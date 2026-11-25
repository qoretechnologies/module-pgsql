#!/bin/sh
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
# Source before building or testing. Adapted from qore commit
# 22a9b5c721. Returning runs the full job; exiting skips its already-tested work.

if [ "${CI_COMMIT_BRANCH:-}" != develop ] || [ "${CI_PIPELINE_SOURCE:-}" != push ] \
        || [ "${QORE_CI_FORCE_FULL:-}" = 1 ] || [ "${QORE_TREE_TESTED_CHECKED:-}" != 1 ]; then
    return 0
fi
case "${QORE_TREE_TESTED_IN:-}" in
    ''|*[!0-9]*|0) return 0 ;;
esac
case "${QORE_TREE_TESTED_SHA:-}" in
    ''|*[!0-9a-f]*) return 0 ;;
esac
if [ "${#QORE_TREE_TESTED_SHA}" -ne 40 ] && [ "${#QORE_TREE_TESTED_SHA}" -ne 64 ]; then
    return 0
fi
current_tree=$(git rev-parse 'HEAD^{tree}' 2>/dev/null) || return 0
if [ "${QORE_TREE_TESTED_IN}" = "${CI_PIPELINE_ID:-}" ] \
        || [ "${QORE_TREE_TESTED_TREE:-}" != "$current_tree" ] \
        || [ -z "${CI_PROJECT_URL:-}" ]; then
    return 0
fi

tested_url="${CI_PROJECT_URL}/-/pipelines/${QORE_TREE_TESTED_IN}"
echo "this tree passed both platform jobs in pipeline ${QORE_TREE_TESTED_IN}: ${tested_url}"
exit 0
