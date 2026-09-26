#!/usr/bin/env bash
# Every SYS_* number the dispatcher cannot reach must SAY it is retired.
#
# Stage 10-abi froze the surface and drew its own lesson from its opening
# paragraph: "a description nothing checks is a description that goes stale".
# AB-1 already pins the BEHAVIOUR — every number the dispatcher can see answers
# NOT_SUPPORTED.  Nothing pinned the DECLARATION, and eleven numbers drifted:
# retired in fact, documented as live, three of them with no retirement note
# anywhere in the tree and one still describing a handle table deleted in
# Stage 4.
#
# Reachable means: a case in the numbered dispatcher, or an INV_ label, or a
# sys_<name>() handler.  Anything else must carry RETIRED/reserved within the
# fifteen lines above its #define.
set -u
HDR="kernel/include/iris/syscall.h"
fail=0

while read -r name; do
    base="${name#SYS_}"
    lower="$(printf '%s' "$base" | tr 'A-Z' 'a-z')"
    if grep -qE "case $name:" kernel/core/syscall/syscall_dispatch.c 2>/dev/null; then continue; fi
    if grep -rqiE "INV_${base}\b|sys_${lower}\(" kernel/core/syscall/ 2>/dev/null; then continue; fi
    # The marker may be INLINE on the define itself -- that is the older style
    # in this header ("#define SYS_HANDLE_TRANSFER 23  /* RETIRED A1.8 ... */").
    if grep -E "^#define $name\\b" "$HDR" | grep -qiE "RETIRED|reserved|NOT_SUPPORTED"; then continue; fi
    # Otherwise, scoped to THIS define's own block: from the previous "#define SYS_" to
    # this one.  A fifteen-line window borrows the neighbour's marker, which is
    # how the first version of this gate passed a define whose marker had been
    # deleted -- SYS_HANDLE_TYPE sits just above SYS_HANDLE_SAME_OBJECT.
    if awk -v want="#define $name" '
        /^#define SYS_[A-Z0-9_]+/ { if (index($0, want) == 1) { print blk; exit } blk=""; next }
        { blk = blk "\n" $0 }
    ' "$HDR" | grep -qiE "RETIRED|reserved|NOT_SUPPORTED"; then continue; fi
    echo "[abi] $name is unreachable and is not declared RETIRED in $HDR"
    fail=1
done < <(grep -oE "^#define SYS_[A-Z0-9_]+" "$HDR" | awk '{print $2}')

if [ "$fail" -ne 0 ]; then
    echo "[abi] RESULT: FAIL — a frozen surface that lies about itself is not frozen"
    exit 1
fi
echo "[abi] RESULT: OK (every unreachable SYS_* number declares its retirement)"
