#!/bin/sh

# The functions that handle credentials do not trace them: they turn tracing off and restore the caller's state.
# ci_source_env() is in ci_env.sh, which the job sources first.

# pg_error_codes <output> [<text to print if the output has no code>]
#
# Prints the exception code of the output of a failed command, and nothing else of it: the description of an exception
# can contain the connection string.  Only the codes that these commands raise are accepted, so that a line of a
# multi-line value that merely looks like a code is not reported.
pg_error_codes() {
    local code
    code=$(printf '%s\n' "$1" | sed -n -E \
        's/^( \* <[a-z-]+>:-?[0-9]+: )?(DBI:PGSQL:(ARRAY-BIND-ERROR|ARRAY-ERROR|BIND-ERROR|CONNECTION-ERROR|COPY-ERROR|ENCODING-ERROR|ERROR|EXEC-EXCEPTION|TIMEZONE-ERROR|TYPE-ERROR|UNKNOWN-CHARACTER-SET)|DATASOURCE-PARSE-ERROR|PARSE-ERROR|PARSE-EXCEPTION|LOAD-MODULE-ERROR): .*/\2/p' | head -n 1)
    printf '%s\n' "${code:-${2:-}}"
}

# Checks that the connection in QORE_DB_CONNSTR_PGSQL works; it is read from the environment, so it is on no command
# line, and a failure reports only its exception code.
pg_check_connection() {
    { local pg_check_flags=$-; set +x; } 2>/dev/null
    local out status=0
    out=$(qore -nX '(new Datasource(ENV.QORE_DB_CONNSTR_PGSQL)).getServerVersion()' 2>&1) || status=$?
    if [ "${status}" != 0 ]; then
        echo "ERROR: cannot connect to the test database: $(pg_error_codes "${out}" "(no error code in the output)")" >&2
    else
        echo "verified the connection to the test database"
    fi
    {
        case $pg_check_flags in
            *x*) set -x ;;
        esac
        return "${status}"
    } 2>/dev/null
}

# Sets PGPASSWORD from POSTGRES_PASSWORD_MAP for RUNNER_HOST.  The map is read from the environment, so it is neither in
# the program text nor on a command line, and a failure reports only its exception code.
pg_read_map_password() {
    { local pg_map_flags=$-; set +x; } 2>/dev/null
    local out status=0
    # the markers separate the value from anything else on the merged output; no temporary file holds the output
    out=$(PG_MAP_HOST="${RUNNER_HOST:=pgsql}" POSTGRES_PASSWORD_MAP="${POSTGRES_PASSWORD_MAP}" qore -l Util -ne \
        'printf("<<PGMAP>>%s<<END>>", parse_to_qore_value(ENV.POSTGRES_PASSWORD_MAP){ENV.PG_MAP_HOST});' 2>&1) || status=$?
    PGPASSWORD=
    if [ "${status}" = 0 ]; then
        case "${out}" in
            *"<<PGMAP>>"*"<<END>>"*)
                PGPASSWORD=${out#*"<<PGMAP>>"}
                PGPASSWORD=${PGPASSWORD%"<<END>>"*}
                ;;
            *)
                status=1
                ;;
        esac
    fi
    if [ "${status}" != 0 ]; then
        echo "ERROR: cannot read the PostgreSQL password map: $(pg_error_codes "${out}" "(no error code in the output)")" >&2
    fi
    {
        case $pg_map_flags in
            *x*) set -x ;;
        esac
        return "${status}"
    } 2>/dev/null
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
    local pg_env="${ENV_FILE:-/tmp/env.sh}"
    local PG_TEST_PASS=qore
    # add env vars to environment file and load it
    # NOTE: must convert to lower case only, or the psql commands below will fail
    user=qore_test_`qore -lUtil -ne 'printf("%s", get_random_string(10));' | tr A-Z a-z`
    ci_env_export "${pg_env}" QORE_DB_USER "${user}"
    ci_env_export "${pg_env}" QORE_DB_PASS "${PG_TEST_PASS}"
    ci_env_export "${pg_env}" QORE_DB_NAME "${user}"
    ci_env_export "${pg_env}" QORE_DB_HOST "${RUNNER_HOST:=pgsql}"
    systemdb="pgsql:${user}/${PG_TEST_PASS}@${user}%${RUNNER_HOST:=pgsql}"
    ci_env_export "${pg_env}" QORE_DB_CONNSTR_PGSQL "${systemdb}"
    ci_env_export "${pg_env}" QORE_DB_CONNSTR "${systemdb}"

    # if we have a password for the local postgres user, then use it and connect to localhost as well
    if [ -n "${POSTGRES_PASSWORD_MAP}" ]; then
        pg_read_map_password
        if [ -n "${PGPASSWORD}" ]; then
            ci_env_export "${pg_env}" PGPASSWORD "${PGPASSWORD}"
            ci_env_export "${pg_env}" PSQL_ARGS "-h ${RUNNER_HOST:=pgsql}"
        fi
    fi

    ci_source_env "${pg_env}"

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
create user ${QORE_DB_USER} password '${PG_TEST_PASS}' createrole;
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

