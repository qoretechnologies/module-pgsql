#!/bin/bash
# Copyright (C) 2026 Qore Technologies, s.r.o.
#
# ci_source_env() (ci_env.sh) sources a file that holds credentials.  This checks, in bash and in sh, that:
# - the values never reach a shell trace, and neither does the toggling of the trace
# - the caller's tracing state is kept, whether it was on or off, also when the file nests another ci_source_env call
# - the variables are exported to child processes
# - "set -e" stays in effect inside the file, and the status of the file is returned without it
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
sentinel=FAKE-SENTINEL-NOT-A-CREDENTIAL
work=$(mktemp -d) || exit 1
if [ -z "$work" ] || [ ! -d "$work" ]; then
    echo "FAIL: cannot create a temporary directory"
    exit 1
fi
trap 'rm -rf "$work"' EXIT
fail=0

printf 'export CI_ENV_TEST_VALUE=%s\n' "$sentinel" > "$work/env.sh"
printf '. "%s/ci_env.sh"\nci_source_env "%s/env.sh"\nexport CI_ENV_TEST_NESTED=1\n' "$here" "$work" > "$work/nested.sh"
printf 'false\nexport CI_ENV_TEST_AFTER=1\n' > "$work/fails.sh"
printf 'export CI_ENV_TEST_VALUE=%s\nreturn 7\n' "$sentinel" > "$work/status.sh"
# the expected value is read from a file: on the command line, it would be in the caller's trace
printf '%s\n' "$sentinel" > "$work/expected"

# expect <shell> <name> <expected status, or "nonzero"> <expected output> <script>: the script's stdout is compared,
# its trace is searched
expect() {
    local shell=$1 name="$1: $2" expected_rc=$3 expected_out=$4 script=$5 out rc rc_ok=1
    out=$("$shell" -c ". \"$here/ci_env.sh\"; $script" 2> "$work/trace")
    rc=$?
    if [ "$expected_rc" = nonzero ]; then
        [ "$rc" != 0 ] || rc_ok=
    else
        [ "$rc" = "$expected_rc" ] || rc_ok=
    fi
    if [ -z "$rc_ok" ] || [ "$out" != "$expected_out" ]; then
        echo "FAIL: $name: expected status $expected_rc and output '$expected_out', got $rc and '$out'"
        fail=1
    elif grep -q -F "$sentinel" "$work/trace"; then
        echo "FAIL: $name: the value is in the trace"
        fail=1
    elif grep -q -E 'set [-+]x|ci_source_env_' "$work/trace"; then
        echo "FAIL: $name: the toggling of the trace is in the trace"
        fail=1
    else
        echo "ok: $name"
    fi
}

child='bash -c "[ \"\$CI_ENV_TEST_VALUE\" = \"\$(cat '$work'/expected)\" ] && echo exported"'
flag='case $- in *x*) echo on ;; *) echo off ;; esac'

for shell in bash sh; do
    expect $shell "tracing stays on" 0 on "set -x; ci_source_env '$work/env.sh'; { $flag; } 2>/dev/null"
    expect $shell "tracing stays off" 0 off "ci_source_env '$work/env.sh'; $flag"
    expect $shell "exported to a child with tracing on" 0 exported "set -x; ci_source_env '$work/env.sh'; $child"
    expect $shell "exported to a child with tracing off" 0 exported "ci_source_env '$work/env.sh'; $child"
    expect $shell "a nested call keeps tracing on" 0 on "set -x; ci_source_env '$work/nested.sh'; { $flag; } 2>/dev/null"
    expect $shell "a nested call keeps tracing off" 0 off "ci_source_env '$work/nested.sh'; $flag"
    expect $shell "a nested call exports" 0 exported "set -x; ci_source_env '$work/nested.sh'; $child"
    expect $shell "set -e applies inside the file" 1 "" "set -e; ci_source_env '$work/fails.sh'; echo continued"
    expect $shell "set -e applies to a missing file" nonzero "" \
        "set -e; ci_source_env '$work/missing.sh' 2>/dev/null; echo continued"
    expect $shell "the status is returned without set -e" 0 "7 on" \
        "set -x; ci_source_env '$work/status.sh'; { rc=\$?; echo \"\$rc \$(case \$- in *x*) echo on ;; *) echo off ;; esac)\"; } 2>/dev/null"
done

# setup_postgres_on_host (postgres_lib.sh) under "set -x", with stub tools: for each value, nothing of it reaches the
# output, the trace or a command line, it is neither split nor executed, and it reaches a child process unchanged
mkdir -p "$work/bin" "$work/tmp" "$work/badsed"
cat > "$work/bin/qore" <<'STUB'
#!/bin/sh
case "$*" in
    *FAKE-SENTINEL-NOT-A-CREDENTIAL*)
        : > "${STUB_DIR}/args-leak"
        exit 99 ;;
esac
case "$*" in
    *get_random_string*) printf abcdefghij; exit 0 ;;
    *parse_to_qore_value*)
        if [ "${STUB_FAIL:-}" = map ]; then
            echo "PARSE-ERROR: bad value in ${POSTGRES_PASSWORD_MAP}" >&2
            exit 3
        fi
        printf '<<PGMAP>>%s<<END>>' "${POSTGRES_PASSWORD_MAP#pgsql=}"
        exit 0 ;;
    *getServerVersion*)
        case "${STUB_FAIL:-}" in
            connection)
                echo "DATASOURCE-PARSE-ERROR: missing database name in 'pgsql:u/${POSTGRES_PASSWORD_MAP}'" >&2
                exit 4 ;;
            noisy)
                echo "180000 ${POSTGRES_PASSWORD_MAP}"
                exit 0 ;;
        esac
        echo 180000
        exit 0 ;;
esac
exit 0
STUB
cat > "$work/bin/psql" <<'STUB'
#!/bin/sh
case "$*" in
    *FAKE-SENTINEL-NOT-A-CREDENTIAL*)
        : > "${STUB_DIR}/args-leak"
        exit 99 ;;
    *"select 1"*) ;;
    *) cat > /dev/null ;;
esac
exit 0
STUB
printf '#!/bin/sh\nexit 1\n' > "$work/badsed/sed"
chmod +x "$work/bin/qore" "$work/bin/psql" "$work/badsed/sed"

# run_lib <shell> <stub failure> <value> <script>: runs the script with the helpers loaded and the stubs first in PATH
run_lib() {
    (cd "$work" && PATH="$work/bin:$PATH" STUB_DIR="$work" POSTGRES_PASSWORD_MAP="pgsql=$3" STUB_FAIL="$2" \
        RUNNER_HOST=pgsql ENV_FILE="$work/setup-env.sh" TMPDIR="$work/tmp" "$1" -c "
        . \"$here/ci_env.sh\"
        . \"$here/postgres_lib.sh\"
        $4" 2>&1)
}

# check_output <name> <status> <expected status> <expected output> <output>
check_output() {
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1: expected status $3, got $2"
        fail=1
    elif [ -e "$work/args-leak" ]; then
        echo "FAIL: $1: the value is on a command line"
        fail=1
    elif [ -e "$work/executed" ]; then
        echo "FAIL: $1: the value was executed"
        fail=1
    elif [ -n "$(ls -A "$work/tmp")" ]; then
        echo "FAIL: $1: a temporary file was left behind"
        fail=1
    elif printf '%s' "$5" | grep -q -F "$sentinel"; then
        echo "FAIL: $1: the value is in the output or the trace"
        fail=1
    elif ! printf '%s\n' "$5" | grep -q -F -- "$4"; then
        echo "FAIL: $1: the expected result is not in the output"
        fail=1
    else
        echo "ok: $1"
    fi
}

# check_setup <shell> <name> <expected status> <expected output> <value> [<stub failure>]
check_setup() {
    local out rc
    : > "$work/setup-env.sh"
    rm -f "$work/executed" "$work/args-leak"
    printf '%s' "$5" > "$work/setup-expected"
    out=$(run_lib "$1" "${6:-}" "$5" "
        set -e
        set -x
        setup_postgres_on_host
        { case \$- in *x*) echo tracing-on ;; *) echo tracing-off ;; esac; } 2>/dev/null
        bash -c '[ \"\$PGPASSWORD\" = \"\$(cat setup-expected)\" ] && echo exported'
        ")
    rc=$?
    check_output "$1: setup: $2" "$rc" "$3" "$4" "$out"
}

# check_direct <shell> <function> <stub failure> <expected status> <tracing: on or off>: the function keeps the value
# out of a trace and restores the caller's tracing state, on success and on failure
check_direct() {
    local out rc start=
    if [ "$5" = on ]; then
        start="set -x"
    fi
    rm -f "$work/executed" "$work/args-leak"
    out=$(run_lib "$1" "$3" "$sentinel" "
        $start
        $2
        rc=\$?
        { case \$- in *x*) t=on ;; *) t=off ;; esac; echo \"result=\$rc tracing-\$t\"; } 2>/dev/null
        ")
    rc=$?
    check_output "$1: $2: failure mode '${3:-none}', tracing $5" "$rc" 0 "result=$4 tracing-$5" "$out"
}

newline_value="first line
$sentinel
last line"

for shell in bash sh; do
    check_setup $shell "a plain value" 0 exported "$sentinel"
    check_setup $shell "tracing is restored" 0 tracing-on "$sentinel"
    check_setup $shell "white space" 0 exported "a  b $sentinel c"
    check_setup $shell "single quotes" 0 exported "it's '$sentinel'"
    check_setup $shell "double quotes" 0 exported "\"$sentinel\""
    check_setup $shell "a command substitution" 0 exported "\$(touch executed)$sentinel"
    check_setup $shell "backticks" 0 exported "\`touch executed\`$sentinel"
    check_setup $shell "a semicolon" 0 exported "x;touch executed;$sentinel"
    check_setup $shell "backslashes" 0 exported "a\\b\\n\\\\$sentinel\\"
    check_setup $shell "newlines" 0 exported "$newline_value"
    check_setup $shell "a failed map keeps its code only" 3 PARSE-ERROR "$sentinel" map
    check_setup $shell "a failed connection keeps its code only" 4 DATASOURCE-PARSE-ERROR "$sentinel" connection

    for trace in on off; do
        check_direct $shell pg_check_connection noisy 0 $trace
        check_direct $shell pg_check_connection connection 4 $trace
        check_direct $shell pg_read_map_password "" 0 $trace
        check_direct $shell pg_read_map_password map 3 $trace
    done

    # a value that cannot be quoted is not written, whether or not the caller uses "set -e"
    : > "$work/sed-env.sh"
    out=$(PATH="$work/badsed:$PATH" "$shell" -c ". \"$here/ci_env.sh\"
        ci_env_export \"$work/sed-env.sh\" CI_ENV_TEST_VALUE \"\$CI_ENV_TEST_INPUT\"
        echo \"status=\$?\"" 2>&1 < /dev/null)
    if ! printf '%s\n' "$out" | grep -q '^status=1$' || [ -s "$work/sed-env.sh" ]; then
        echo "FAIL: $shell: a failed quote must fail and write nothing"
        fail=1
    else
        echo "ok: $shell: a failed quote fails and writes nothing"
    fi
done
exit $fail
