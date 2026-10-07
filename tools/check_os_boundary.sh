#!/bin/sh
# Checks the OS boundary: only the OS libraries (src/os/) may include
# operating-system headers. Everything else is plain C99 plus include/os.h.
# (Calls to POSIX/GNU functions are caught by the compiler instead: the core
# is built with -std=c99, no feature-test macros, and
# -Werror=implicit-function-declaration.)
#
# Usage: tools/check_os_boundary.sh [source-root]    — exit 1 on violations.

root="${1:-$(dirname "$0")/..}"
cd "$root" || exit 2

# POSIX, Linux, BSD/macOS and Windows headers.
forbidden='unistd|fcntl|termios|poll|pty|util|dirent|libgen|pthread|strings|spawn|dlfcn'
forbidden="$forbidden|sys/[a-z_/]+|windows|winsock2?|io|direct|process|conio|shellapi|shlobj"
forbidden="$forbidden|mach/[a-z_/]+|linux/[a-z_/]+"

# Vendored, self-contained portable C libraries are exempt: cJSON selects
# its own export macros on Windows, lisp.h maps files with mmap() unless
# LISP_NO_MMAP (which meson.build defines) is set.
violations=$(grep -nE "^[[:space:]]*#[[:space:]]*include[[:space:]]*<($forbidden)\.h>" \
    main.c src/*.c include/*.h 2>/dev/null \
    | grep -v -e '^src/cJSON\.c:' -e '^include/cJSON\.h:' -e '^include/lisp\.h:')

if [ -n "$violations" ]; then
    echo "OS headers outside src/os/ (use include/os.h instead):" >&2
    echo "$violations" >&2
    exit 1
fi

# Platform #ifdefs belong in the OS layer too.
ifdefs=$(grep -nE '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif).*(_WIN32|__APPLE__|__linux__|__unix__|_MSC_VER|__MINGW)' \
    main.c src/*.c include/*.h 2>/dev/null \
    | grep -v -e '^src/cJSON\.c:' -e '^include/cJSON\.h:' -e '^include/lisp\.h:')
if [ -n "$ifdefs" ]; then
    echo "Platform #ifdefs outside src/os/ (put the difference behind include/os.h):" >&2
    echo "$ifdefs" >&2
    exit 1
fi

echo "OS boundary OK: no OS headers or platform #ifdefs outside src/os/"
