#!/bin/bash

set -e
set -x

ENV_FILE=/tmp/env.sh

. ${ENV_FILE}

apk add postgresql-client

. test/docker_test/postgres_lib.sh
setup_postgres_on_host

# setup MODULE_SRC_DIR env var
cwd=`pwd`
if [ -z "${MODULE_SRC_DIR}" ]; then
    if [ -e "$cwd/src/pgsql.cpp" ]; then
        MODULE_SRC_DIR=$cwd
    else
        MODULE_SRC_DIR=$WORKDIR/module-pgsql
    fi
fi
echo "export MODULE_SRC_DIR=${MODULE_SRC_DIR}" >> ${ENV_FILE}

echo "export QORE_UID=1000" >> ${ENV_FILE}
echo "export QORE_GID=1000" >> ${ENV_FILE}

. ${ENV_FILE}

export MAKE_JOBS=4

# build module and install
echo && echo "-- building module --"
mkdir -p ${MODULE_SRC_DIR}/build
cd ${MODULE_SRC_DIR}/build
cmake .. -DCMAKE_BUILD_TYPE=debug -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}
make -j${MAKE_JOBS}
make install

# add Qore user and group
if ! grep -q "^qore:x:${QORE_GID}" /etc/group; then
    addgroup -g ${QORE_GID} qore
fi
if ! grep -q "^qore:x:${QORE_UID}" /etc/passwd; then
    adduser -u ${QORE_UID} -D -G qore -h /home/qore -s /bin/bash qore
fi

# own everything by the qore user
chown -R qore:qore ${MODULE_SRC_DIR}

# run the tests
export QORE_MODULE_DIR=${MODULE_SRC_DIR}/qlib:${QORE_MODULE_DIR}
cd ${MODULE_SRC_DIR}
# the tests must load the module that this job built and installed, not one that the image already has
loaded=$(gosu qore:qore qore -l pgsql -e 'printf("%s", get_module_hash().pgsql.filename);')
if ! grep -qxF "${loaded}" ${MODULE_SRC_DIR}/build/install_manifest.txt; then
    echo "ERROR: the tests would load '${loaded}', which this job did not install" >&2
    exit 1
fi
echo "-- the tests load ${loaded}"

for test in test/*.qtest; do
    gosu qore:qore qore -b --enable-debug $test -vv
    RESULTS="$RESULTS $?"
    date
done

gosu qore:qore python3 -W error test/test_connect_failover.py -v

# check the results
for R in $RESULTS; do
    if [ "$R" != "0" ]; then
        exit 1 # fail
    fi
done

cleanup_postgres_on_host
