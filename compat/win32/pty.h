/* Windows stand-in for <pty.h> — see compat/win32/README.md.
 * There is no forkpty() on Windows: terminal panes use ConPTY through
 * belugaPtySpawn() instead (src/pty_session.c, src/platform_win32.c). */
#ifndef BELUGA_COMPAT_PTY_H_
#define BELUGA_COMPAT_PTY_H_
#endif
