# Copyright (C) 2026 Qore Technologies, s.r.o.
#
# sourced by the CI scripts: the environment file holds credentials, so it must never be sourced under "set -x"
# (works in bash, dash and busybox ash)

# ci_source_env <file>
#
# Sources <file> with shell tracing off and restores the caller's tracing state afterwards.  The file is not sourced
# as part of an AND-OR list, so that "set -e" stays in effect inside it.  Returns the status of the sourced file.
# The file must set its variables with "export": it runs inside this function, so "declare -x" would make them local.
# The shell's own error messages quote the text of the file, so they are not shown; a file that cannot be parsed or
# read is reported without its content.
ci_source_env() {
    { local ci_source_env_flags=$-; set +x; } 2>/dev/null
    local ci_source_env_status=0
    if ! "${BASH:-sh}" -n "$1" 2>/dev/null; then
        echo "ERROR: the CI environment file cannot be read or parsed" >&2
    fi
    . "$1" 2>/dev/null
    ci_source_env_status=$?
    {
        case $ci_source_env_flags in
            *x*) set -x ;;
        esac
        return $ci_source_env_status
    } 2>/dev/null
}

# ci_env_export <file> <name> <value>
#
# Appends an export of <name> to the environment file, with <value> quoted for the shell, so that white space and shell
# metacharacters in it are neither split nor executed when the file is sourced.
ci_env_export() {
    { local ci_env_export_flags=$-; set +x; } 2>/dev/null
    local ci_env_export_value ci_env_export_status=0
    ci_env_export_value=$(printf '%s' "$3" | sed "s/'/'\\\\''/g")
    printf "export %s='%s'\n" "$2" "${ci_env_export_value}" >> "$1" || ci_env_export_status=$?
    {
        case $ci_env_export_flags in
            *x*) set -x ;;
        esac
        return $ci_env_export_status
    } 2>/dev/null
}
