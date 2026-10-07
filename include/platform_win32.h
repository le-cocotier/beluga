/**
 * @file platform_win32.h
 * @brief What beluga needs from POSIX, provided on Windows.
 * @details Force-included (gcc -include) into every translation unit of a
 * Windows build by meson.build; never seen on Linux or macOS. Together with
 * the stand-in headers in compat/win32/ (termios.h, sys/select.h, poll.h,
 * ...) it lets the Unix-facing sources compile unchanged, so the Windows
 * port lives here and in src/platform_win32.c instead of in #ifdefs
 * scattered through the editor. The places that do need their own Windows
 * code — starting processes, since there is no fork() — are marked
 * `#ifdef _WIN32` and call the belugaSpawn()/belugaPtySpawn() API below.
 *
 * How the Unix model maps onto Windows:
 * - Terminal: the console, put in VT mode. Output escape sequences are
 *   interpreted natively (ENABLE_VIRTUAL_TERMINAL_PROCESSING); keys come
 *   in as the same byte sequences an xterm sends
 *   (ENABLE_VIRTUAL_TERMINAL_INPUT), so the key parser in utf8.c is shared.
 *   read(0) keeps raw mode's VMIN=0/VTIME=1 timing: up to 100ms for the
 *   next byte, then 0.
 * - select()/poll(): over the console and anonymous pipes, by polling
 *   pipes with PeekNamedPipe while waiting on the console handle.
 * - O_NONBLOCK: tracked per descriptor; read() on an empty non-blocking
 *   pipe fails with EAGAIN instead of blocking.
 * - Processes: CreateProcess, each child in a Job object, so kill(-pid)
 *   ends the whole tree the way killing a process group does on Unix.
 * - Terminal panes: ConPTY. The pane's descriptor reads the pseudoconsole's
 *   output; write() and ioctl(TIOCSWINSZ) on it are routed to its input
 *   pipe and to ResizePseudoConsole.
 * - Paths: always handed to the editor with forward slashes (getcwd,
 *   realpath, $HOME), which Windows accepts everywhere, so the editor's
 *   '/'-based path code keeps working. IS_ABS_PATH() in define.h knows
 *   about drive letters.
 * - Text: the build embeds a manifest setting the process code page to
 *   UTF-8 (Windows 10 1903+), so the C runtime's narrow file APIs take the
 *   same UTF-8 strings as on Unix. Files are opened in binary mode
 *   (_fmode), so nothing is CRLF-translated behind the editor's back.
 */
#ifndef BELUGA_PLATFORM_WIN32_H_
#define BELUGA_PLATFORM_WIN32_H_

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // keeps winsock's fd_set/select out of <windows.h>
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10: ConPTY, VT console modes
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006 // Windows 10 1809, which introduced ConPTY
#endif

// System headers first, so the redirecting macros further down rename
// only the calls in beluga's own code, never the CRT's declarations.
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// MinGW's PATH_MAX is MAX_PATH (260): too tight for the buffers sized by
// it (a path plus quoting, a URI prefix, ...).
#undef PATH_MAX
#define PATH_MAX 1024

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif

// Signals Windows doesn't have. kill() treats every one of them except
// SIGWINCH (and 0) as "terminate".
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif

#ifndef S_ISLNK
#define S_ISLNK(m) 0 // symlinks are reported as what they point to
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) 0
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & _S_IFMT) == _S_IFIFO)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) 0
#endif
#ifndef S_ISUID // no setuid/setgid/sticky bits on Windows
#define S_ISUID 0
#endif
#ifndef S_ISGID
#define S_ISGID 0
#endif
#ifndef S_ISVTX
#define S_ISVTX 0
#endif

// ─── POSIX functions, redirected ─────────────────────────────────────────────

ssize_t beluga_read(int fd, void *buf, size_t count);
ssize_t beluga_write(int fd, const void *buf, size_t count);
int beluga_close(int fd);
int beluga_pipe(int fds[2]);
int beluga_fcntl(int fd, int cmd, ...);
#define read(fd, buf, n) beluga_read((fd), (buf), (n))
#define write(fd, buf, n) beluga_write((fd), (buf), (n))
#define close(fd) beluga_close(fd)
#define pipe(fds) beluga_pipe(fds)
#define fcntl(...) beluga_fcntl(__VA_ARGS__)

int kill(pid_t pid, int sig);
#define setpgid(pid, pgid) ((void)(pid), (void)(pgid)) // Job objects instead
#define setsid() ((void)0)

typedef void (*beluga_sighandler_t)(int);
// The C runtime aborts on a signal number it doesn't know (SIGPIPE, ...);
// this ignores those instead.
beluga_sighandler_t beluga_signal(int sig, beluga_sighandler_t handler);
#define signal(sig, handler) beluga_signal((sig), (handler))

char *beluga_getcwd(char *buf, size_t size);
char *beluga_realpath(const char *path, char *resolved);
#define getcwd(buf, size) beluga_getcwd((buf), (size))
#define realpath(path, resolved) beluga_realpath((path), (resolved))
#define lstat(path, st) stat((path), (st))
#define readlink(path, buf, size) ((void)(path), (void)(buf), (void)(size), (ssize_t)-1)

int beluga_setenv(const char *name, const char *value, int overwrite);
int beluga_unsetenv(const char *name);
#define setenv(n, v, o) beluga_setenv((n), (v), (o))
#define unsetenv(n) beluga_unsetenv(n)

ssize_t beluga_getline(char **lineptr, size_t *n, FILE *stream);
void *beluga_memmem(const void *haystack, size_t haystack_len,
                    const void *needle, size_t needle_len);
char *beluga_strndup(const char *s, size_t n);
#define getline(l, n, f) beluga_getline((l), (n), (f))
#define memmem(h, hl, n, nl) beluga_memmem((h), (hl), (n), (nl))
#define strndup(s, n) beluga_strndup((s), (n))

// MinGW only declares localtime_r for POSIX-thread builds.
#define localtime_r(t, result) (localtime_s((result), (t)) == 0 ? (result) : NULL)

// msvcrt.dll's strftime has no %e (aborts on it); this maps it to %d.
size_t beluga_strftime(char *s, size_t max, const char *format, const struct tm *tm);
#define strftime(s, m, f, t) beluga_strftime((s), (m), (f), (t))

// ─── Windows-only API used by the #ifdef _WIN32 sections ────────────────────

// Flags for belugaSpawn().
#define BELUGA_SPAWN_NO_PROMPT 0x1 // GIT_TERMINAL_PROMPT=0 in the child's
                                   // environment: the Windows counterpart of
                                   // the setsid() the Unix code uses to make
                                   // password prompts fail instead of hang

/**
 * Starts argv[0] — searched on PATH, with PATHEXT, so .cmd/.bat shims such
 * as npm-installed language servers work too — with the given descriptors
 * as its stdin/stdout/stderr (-1: the NUL device). The child gets a hidden
 * console of its own and its own Job object. Returns its pid, usable with
 * waitpid() and kill(), or -1 with errno set (ENOENT: no such program).
 */
pid_t belugaSpawn(char *const argv[], int stdin_fd, int stdout_fd, int stderr_fd,
                  unsigned flags);

// Same, but `command` is a cmd.exe command line (the Windows counterpart
// of `/bin/sh -c command`).
pid_t belugaSpawnShell(const char *command, int stdin_fd, int stdout_fd,
                       int stderr_fd, unsigned flags);

/**
 * Starts `shell` (NULL: $BELUGA_SHELL, else pwsh, else Windows PowerShell,
 * else %COMSPEC%) attached to a new ConPTY of rows×cols. Returns a
 * descriptor that reads the terminal's output and accepts write() (keys)
 * and ioctl(TIOCSWINSZ) (resize); close() tears the pseudoconsole down.
 * *pid receives the shell's pid. -1 on failure.
 */
int belugaPtySpawn(const char *shell, int rows, int cols, pid_t *pid);

// The Windows clipboard, as UTF-8 with LF line endings (CRLF is used on
// the clipboard itself, as Windows apps expect). Get returns malloc'd
// text, or NULL if the clipboard holds no text.
char *belugaClipboardGet(void);
int belugaClipboardSet(const char *text, size_t len);

// File operations for the Lisp builtins dired uses on Windows, where there
// are no rm/mv/cp/mkdir programs to run. Each returns 0 on success, or an
// error code with a message in `err`.
//  - delete: removes files and directories (recursively), permanently.
//  - move/copy: like mv / cp -R: into `dest` when it's an existing
//    directory, otherwise to `dest` itself (one source only).
#define BELUGA_FILEOP_DELETE 1
#define BELUGA_FILEOP_MOVE 2
#define BELUGA_FILEOP_COPY 3
int belugaFileOperation(int op, const char *const *paths, int count, const char *dest,
                        char *err, size_t err_size);
// mkdir -p.
int belugaMakeDirectories(const char *path, char *err, size_t err_size);

// The PowerShell to run shell commands with: "pwsh" if installed, else
// "powershell" (Windows PowerShell, always there).
const char *belugaPowerShell(void);
// `script` as PowerShell's -EncodedCommand wants it (base64 of UTF-16LE):
// passed that way, no layer of command-line quoting can alter it.
// malloc'd; NULL on failure.
char *belugaPowerShellEncode(const char *script);

#endif // _WIN32
#endif // BELUGA_PLATFORM_WIN32_H_
