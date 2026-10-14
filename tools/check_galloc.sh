#!/bin/sh
# Checks that every allocation goes through galloc (include/galloc.h):
# galloc/gcalloc/grealloc/gstrdup to allocate, gfree to release, so the
# memory inspector (CTRL-x m) and the exit report see every block.
#
# Exempt: src/gutils.c, which implements galloc on top of the C library,
# and lines marked `galloc: raw`, for memory the C library or the OS
# allocates itself (realpath(path, NULL), ...), released right where it is
# copied into a galloc'd block.
#
# Usage: tools/check_galloc.sh [source-root]    — exit 1 on violations.

root="${1:-$(dirname "$0")/..}"
cd "$root" || exit 2

raw='malloc|calloc|realloc|free|strdup|strndup|wcsdup|_strdup|_wcsdup|asprintf|vasprintf|getline|getdelim'

violations=$(grep -nE "(^|[^A-Za-z0-9_.>])($raw)[[:space:]]*\(" \
    main.c src/*.c src/os/*.c include/*.h test/*.c test/fuzz/*.c 2>/dev/null \
    | grep -v -e '^src/gutils\.c:' -e 'galloc: raw' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|/\*|\*)')

if [ -n "$violations" ]; then
    echo "Allocations outside galloc (use galloc/gcalloc/grealloc/gstrdup/gfree):" >&2
    echo "$violations" >&2
    exit 1
fi

echo "galloc OK: every allocation outside src/gutils.c goes through galloc"
