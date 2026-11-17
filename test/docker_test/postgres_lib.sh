#!/bin/sh

start_postgres() {
    docker run --name=postgres --network=host -e POSTGRES_PASSWORD=qore -e TZ=Europe/Prague -e PGTZ=Europe/Prague -d pgvector/pgvector:pg18

    # wait for PostgreSQL server to start
    printf "waiting on PostgreSQL server: "
    waited=0
    while true; do
        ver=`qore -ne 'try { printf("%s", (new Datasource("pgsql:postgres/qore@postgres%localhost:5432")).getServerVersion()); } catch () {}'`
        if [ -n "$ver" ]; then
            echo ": started server version $ver"
            break
        fi

        # 30 second time limit
        if [ $waited -eq 30 ]; then
            echo && echo "Waited too long for PostgreSQL to start; aborting build."
            exit 1
        fi
        printf .
        # sleep for 1 second
        sleep 1
        waited=$((waited+1))
    done

    export QORE_DB_USER=postgres
    export QORE_DB_PASS=qore
    export QORE_DB_NAME=postgres
    export QORE_DB_HOST=`qore -ne 'printf("%s", (map $1.address, get_netif_list(), $1.family == AF_INET && $1.address !~ /^127/ && $1.address !~ /\.0$/)[0]);'`
    export QORE_DB_CONNSTR_PGSQL=pgsql:${QORE_DB_USER}/${QORE_DB_PASS}@${QORE_DB_NAME}%${QORE_DB_HOST}

    # make sure we can access the DB
    qore -nX "(new Datasource(\"${QORE_DB_CONNSTR_PGSQL}\")).getServerVersion()"

    # enable pgvector extension if available
    psql -Upostgres -h ${QORE_DB_HOST} -c "create extension if not exists vector;" ${QORE_DB_NAME} 2>/dev/null || true
}

# Waits until the PostgreSQL server of the "services" container accepts a connection from the postgres user; fails with a
# message when it does not within PG_READY_ATTEMPTS attempts (default 120), PG_READY_INTERVAL seconds apart (default 1).
# It probes with psql, which the job needs anyway, with the arguments of the job (PSQL_ARGS).
wait_for_postgres_host() {
    attempts=${PG_READY_ATTEMPTS:-120}
    interval=${PG_READY_INTERVAL:-1}
    printf "waiting for the PostgreSQL server: "
    i=1
    while true; do
        # PSQL_ARGS is unquoted on purpose: it holds the option and its value
        if psql -Upostgres ${PSQL_ARGS} -tAc "select 1" postgres >/dev/null 2>&1; then
            echo ": ready after ${i} attempt(s)"
            return 0
        fi
        if [ "${i}" -ge "${attempts}" ]; then
            echo
            echo "The PostgreSQL server (${PSQL_ARGS:-local socket}) did not accept connections after ${attempts} attempts; aborting the build." >&2
            return 1
        fi
        printf .
        sleep "${interval}"
        i=$((i + 1))
    done
}

setup_postgres_on_host() {
    # add env vars to environment file and load it
    # NOTE: must convert to lower case only, or the psql commands below will fail
    user=qore_test_`qore -lUtil -ne 'printf("%s", get_random_string(10));' | tr A-Z a-z`
    echo export QORE_DB_USER=${user} >> /tmp/env.sh
    echo export QORE_DB_PASS=qore >> /tmp/env.sh
    echo export QORE_DB_NAME=${user} >> /tmp/env.sh
    echo export QORE_DB_HOST=${RUNNER_HOST:=pgsql} >> /tmp/env.sh
    systemdb=pgsql:${user}/qore@${user}%${RUNNER_HOST:=pgsql}
    echo export QORE_DB_CONNSTR_PGSQL=${systemdb} >> /tmp/env.sh
    echo export QORE_DB_CONNSTR=${systemdb} >> /tmp/env.sh

    # if we have a password for the local postgres user, then use it and connect to localhost as well
    if [ -n "${POSTGRES_PASSWORD_MAP}" ]; then
        PGPASSWORD=`qore -l Util -ne "printf(\"%s\", parse_to_qore_value(\"${POSTGRES_PASSWORD_MAP}\"){\"${RUNNER_HOST:=pgsql}\"});"`
        if [ -n "${PGPASSWORD}" ]; then
            echo export PGPASSWORD=${PGPASSWORD} >> /tmp/env.sh
            echo export PSQL_ARGS=\"-h ${RUNNER_HOST:=pgsql}\" >> /tmp/env.sh
        fi
    fi

    . /tmp/env.sh

    # the GitLab "services" keyword starts the container but does not wait for the server inside it to finish
    # initializing, so the first connection can be refused
    wait_for_postgres_host

    # create user for test; CREATEROLE lets SqlUtil's PgsqlBoundedTransaction.qtest create (and drop) the role its
    # row-security case needs
    cat <<EOF | psql -Upostgres ${PSQL_ARGS}
create database ${QORE_DB_NAME} encoding = 'utf8';
\connect ${QORE_DB_NAME};
create extension if not exists vector;
create user ${QORE_DB_USER} password 'qore' createrole;
grant create, connect, temp on database ${QORE_DB_NAME} to ${QORE_DB_USER};
grant select on all tables in schema pg_catalog to ${QORE_DB_USER};
grant all on schema public to ${QORE_DB_USER};
EOF
    echo created pgsql user ${QORE_DB_USER} and db ${QORE_DB_NAME}

    # make sure we can access the DB
    qore -nX "(new Datasource(\"${QORE_DB_CONNSTR_PGSQL}\")).getServerVersion()"

    export POSTGRES_HOST=1
}

cleanup_postgres_on_host() {
    if [ "${POSTGRES_HOST}" = "1" ]; then
        # drop postgresql test user
        cat <<EOF | psql -Upostgres ${PSQL_ARGS}
drop owned by ${QORE_DB_USER};
drop database ${QORE_DB_NAME} with (force);
drop role ${QORE_DB_USER};
EOF
        echo dropped pgsql user ${QORE_DB_USER}
    fi
}

