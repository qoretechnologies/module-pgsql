# Copyright (C) 2026 Qore Technologies, s.r.o.
#
# sourced by the CI scripts: the environment file holds credentials, so it must never be sourced under "set -x"
# (works in bash, dash and busybox ash)

# ci_source_env <file>
#
# Sources <file> with shell tracing off and restores the caller's tracing state afterwards.  The file is not sourced
# as part of an AND-OR list, so that "set -e" stays in effect inside it.  Returns the status of the sourced file.
# The file must set its variables with "export": it runs inside this function, so "declare -x" would make them local.
ci_source_env() {
    { local ci_source_env_flags=$-; set +x; } 2>/dev/null
    local ci_source_env_status=0
    . "$1"
    ci_source_env_status=$?
    {
        case $ci_source_env_flags in
            *x*) set -x ;;
        esac
        return $ci_source_env_status
    } 2>/dev/null
}
