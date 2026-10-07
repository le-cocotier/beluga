# Windows stand-ins for POSIX headers

These headers exist only so the Unix-facing sources compile unchanged on
Windows: `meson.build` adds this directory to the include path **only** for
Windows builds, so on Linux and macOS the real system headers are used and
nothing here is ever seen.

Each one declares just enough of its POSIX namesake for what beluga uses; the
implementations live in `src/platform_win32.c`, and `include/platform_win32.h`
(force-included in every Windows translation unit) explains the overall design.
