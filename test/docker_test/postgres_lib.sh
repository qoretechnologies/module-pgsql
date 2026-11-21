#!/bin/sh

# The functions that handle credentials do not trace them: they turn tracing off and restore the caller's state.
# ci_source_env() is in ci_env.sh, which the job sources first.

# pg_error_codes <output> [<text to print if the output has no code>]
#
# Prints the exception code of the output of a failed command, and nothing else of it: the description of an exception
# can contain the connection string.
pg_error_codes() {
    local code
    code=$(printf '%s\n' "$1" | sed -n -E 's/^([A-Z][A-Z0-9_]*((:[A-Z0-9_-]+)|(-[A-Z0-9_]+))+): .*/\1/p' | head -n 1)
    printf '%s\n' "${code:-${2:-}}"
}

# Checks that the connection in QORE_DB_CONNSTR_PGSQL works; it is read from the environment, so it is on no command
# line, and a failure reports only its exception code.
pg_check_connection() {
    local out status=0
    out=$(qore -nX '(new Datasource(ENV.QORE_DB_CONNSTR_PGSQL)).getServerVersion()' 2>&1) || status=$?
    if [ "${status}" != 0 ]; then
        echo "ERROR: cannot connect to the test database: $(pg_error_codes "${out}" "(no error code in the output)")" >&2
        return "${status}"
    fi
    echo "verified the connection to the test database"
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
    { local pg_trace_flags=$-; set +x; } 2>/dev/null
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

    ci_source_env /tmp/env.sh

    # the GitLab "services" keyword starts the container but does not wait for the server inside it to finish
    # initializing, so the first connection can be refused
    wait_for_postgres_host

    # VERBOSITY=sqlstate: an error is reported by its code, without the statement, which holds a password
    # create user for test; CREATEROLE lets SqlUtil's PgsqlBoundedTransaction.qtest create (and drop) the role its
    # row-security case needs
    cat <<EOF | psql -Upostgres -v VERBOSITY=sqlstate ${PSQL_ARGS}
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
    pg_check_connection

    {
        case $pg_trace_flags in
            *x*) set -x ;;
        esac
    } 2>/dev/null

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

