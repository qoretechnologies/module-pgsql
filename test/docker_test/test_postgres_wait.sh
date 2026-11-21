#!/bin/sh
#
# Self-test of wait_for_postgres_host (postgres_lib.sh): the job waits for the PostgreSQL service before it creates the
# test database, with a stub psql that refuses a given number of connections first.
#
# Copyright 2026 Qore Technologies, s.r.o.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "${TEST_DIR}"' EXIT

. "${SCRIPT_DIR}/postgres_lib.sh"

# a psql that fails as many times as the file ${TEST_DIR}/refusals says (and counts its calls), then succeeds
mkdir -p "${TEST_DIR}/bin"
cat > "${TEST_DIR}/bin/psql" <<'STUB'
#!/bin/sh
calls=$(cat "${STUB_DIR}/calls" 2>/dev/null || echo 0)
calls=$((calls + 1))
echo "${calls}" > "${STUB_DIR}/calls"
refusals=$(cat "${STUB_DIR}/refusals")
if [ "${calls}" -le "${refusals}" ]; then
    echo 'psql: error: connection to server at "pgsql" (127.0.0.1), port 5432 failed: Connection refused' >&2
    exit 2
fi
exit 0
STUB
chmod +x "${TEST_DIR}/bin/psql"
export STUB_DIR="${TEST_DIR}"
export PATH="${TEST_DIR}/bin:${PATH}"
export PG_READY_INTERVAL=0
export PSQL_ARGS="-h pgsql"

checks=0
fail() {
    echo "FAILED: $*" >&2
    exit 1
}

# run <refusals> <attempts>: runs the wait with a stub that refuses <refusals> connections; sets rc, calls and out
run() {
    echo 0 > "${TEST_DIR}/calls"
    echo "$1" > "${TEST_DIR}/refusals"
    PG_READY_ATTEMPTS=$2
    export PG_READY_ATTEMPTS
    rc=0
    out=$(wait_for_postgres_host 2>&1) || rc=$?
    calls=$(cat "${TEST_DIR}/calls")
}

# the server is ready at once: one probe
run 0 5
[ "${rc}" -eq 0 ] || fail "a ready server should succeed (rc=${rc}: ${out})"
[ "${calls}" -eq 1 ] || fail "a ready server should be probed once (${calls})"
checks=$((checks + 1))

# the server refuses the first connections, as the job's service did: the wait retries and then succeeds
run 3 10
[ "${rc}" -eq 0 ] || fail "a server that becomes ready should succeed (rc=${rc}: ${out})"
[ "${calls}" -eq 4 ] || fail "three refusals and one success are four probes (${calls})"
case "${out}" in *"ready after 4 attempt(s)"*) ;; *) fail "the wait should say how many attempts it took: ${out}" ;; esac
checks=$((checks + 1))

# the last allowed attempt succeeds
run 4 5
[ "${rc}" -eq 0 ] || fail "success on the last attempt should succeed (rc=${rc}: ${out})"
[ "${calls}" -eq 5 ] || fail "the last attempt is the fifth probe (${calls})"
checks=$((checks + 1))

# a server that never accepts connections: the wait gives up after the allowed attempts, with a message
run 1000 5
[ "${rc}" -ne 0 ] || fail "a server that never accepts connections should fail"
[ "${calls}" -eq 5 ] || fail "the wait should probe exactly the allowed 5 times (${calls})"
case "${out}" in *"did not accept connections after 5 attempts"*) ;; *) fail "the failure should say why: ${out}" ;; esac
checks=$((checks + 1))

# setup_postgres_on_host waits for the server before its first psql (the one that creates the test database)
body=$(sed -n '/^setup_postgres_on_host() {/,/^}/p' "${SCRIPT_DIR}/postgres_lib.sh")
wait_line=$(printf '%s\n' "${body}" | grep -n 'wait_for_postgres_host' | head -1 | cut -d: -f1)
psql_line=$(printf '%s\n' "${body}" | grep -n 'psql -Upostgres' | head -1 | cut -d: -f1)
[ -n "${wait_line}" ] || fail "setup_postgres_on_host does not wait for the server"
[ -n "${psql_line}" ] || fail "setup_postgres_on_host has no psql call: the test needs updating"
[ "${wait_line}" -lt "${psql_line}" ] || fail "setup_postgres_on_host runs psql before it waits for the server"
checks=$((checks + 1))

# only the code of a failed connection check is reported: the rest of its output can hold the connection string
fake_secret=FAKE-SENTINEL-NOT-A-CREDENTIAL
got=$(pg_error_codes "unhandled exception
DATASOURCE-PARSE-ERROR: missing database name in 'pgsql:u/${fake_secret}'" none)
[ "${got}" = DATASOURCE-PARSE-ERROR ] || fail "pg_error_codes should report the exception code"
got=$(pg_error_codes "DBI:PGSQL:ERROR: ${fake_secret}@db: failed" none)
[ "${got}" = "DBI:PGSQL:ERROR" ] || fail "pg_error_codes should report a driver error code"
got=$(pg_error_codes "pgsql:u/${fake_secret}@db" none)
[ "${got}" = none ] || fail "pg_error_codes should report no code for text without one"
checks=$((checks + 1))

cat > "${TEST_DIR}/bin/qore" <<'STUB'
#!/bin/sh
if [ "${STUB_QORE}" = ok ]; then
    echo 180000
    exit 0
fi
msg="DATASOURCE-PARSE-ERROR: missing database name in 'pgsql:u/FAKE-SENTINEL-NOT-A-CREDENTIAL'"
if [ "${STUB_QORE}" = stderr ]; then
    echo "${msg}" >&2
else
    echo "${msg}"
fi
exit 3
STUB
chmod +x "${TEST_DIR}/bin/qore"
for STUB_QORE in stdout stderr; do
    export STUB_QORE
    rc=0
    out=$(pg_check_connection 2>&1) || rc=$?
    [ "${rc}" -eq 3 ] || fail "a failed connection check should return the status of the command (${rc})"
    case "${out}" in
        *"${fake_secret}"*) fail "a failed connection check (${STUB_QORE}) printed the output of the command" ;;
    esac
    case "${out}" in
        *DATASOURCE-PARSE-ERROR*) ;;
        *) fail "a failed connection check (${STUB_QORE}) should report the exception code" ;;
    esac
    checks=$((checks + 1))
done
STUB_QORE=ok
export STUB_QORE
pg_check_connection > /dev/null || fail "a working connection should pass the check"
checks=$((checks + 1))

echo "PostgreSQL readiness wait self-test passed (${checks} checks)"
