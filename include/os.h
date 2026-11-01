/**
 * @file os.h
 * @brief Everything beluga needs from the operating system — and the only
 * way the editor reaches it.
 * @details Two libraries implement this header: libbeluga-os-posix
 * (src/os/os_posix.c: Linux, macOS, BSD) and libbeluga-os-win32
 * (src/os/os_win32.c: Windows 10 1809+). meson.build links the one for the
 * target. Everything else in beluga is plain C99 plus this header: no
 * <unistd.h>, no <windows.h>, no #ifdef per platform — a rule
 * tools/check_os_boundary.sh enforces in CI, and that compiling the core
 * with -std=c99 and no feature-test macros enforces for functions.
 *
 * Conventions, the same on every platform:
 * - Strings are UTF-8. Paths use '/' as separator (Windows accepts it
 *   everywhere; the Windows backend converts what it hands back, so
 *   "C:/Users/me/x.c" is a normal absolute path to the editor).
 * - Memory returned to the caller comes from galloc (include/galloc.h), so
 *   the memory inspector sees it: release it with gfree(), or with the
 *   matching os_free_*() where one exists.
 * - Keyboard input arrives as the byte sequences an xterm sends
 *   ("\x1b[A" for Up, "\x1b[1;5D" for Ctrl+Left, 0x00 for Ctrl+Space, ...),
 *   so there is one key parser for every platform.
 * - Terminal output is UTF-8 text with ANSI/VT escape sequences.
 */
#ifndef BELUGA_OS_H_
#define BELUGA_OS_H_

#include <stddef.h>
#include <stdio.h> // FILE, for os_file_flush()

// Big enough for any path the editor handles (buffers sized by it).
#define OS_PATH_MAX 4096

/***********************************************************************************************************************
 *
 *                                                 Process lifetime and identity
 *
 **********************************************************************************************************************/

/**
 * @brief Prepares the process; call first thing in main().
 * @details POSIX: ignores SIGPIPE (a write to a dead language server or
 * shell must not kill the editor). Windows: binary-mode stdio, UTF-8
 * console output.
 */
void os_init(void);

// "linux", "darwin", "windows", or "unix" — what (system-type) returns.
const char *os_name(void);

// This process's id (the LSP initialize request reports it).
long os_getpid(void);

// What a stop signal asked for (os_take_stop_signal()).
enum {
    OS_STOP_NONE,
    OS_STOP_INTERRUPT, // Ctrl-C: SIGINT; CTRL_C / CTRL_BREAK on Windows
    // The terminal closed, or the process was told to end: SIGHUP,
    // SIGTERM; the console window closing, logoff or shutdown on Windows,
    // which leaves the process a few seconds before it is ended anyway.
    OS_STOP_HANGUP
};

// From now on the stop signals above no longer end the process: each is
// remembered for os_take_stop_signal(). The daemon uses it to save what it
// can and remove its socket before it goes.
void os_catch_stop_signals(void);

// The stop signal that came since the last call (OS_STOP_HANGUP first when
// both did), or OS_STOP_NONE.
int os_take_stop_signal(void);

// The running program's own path (galloc'd), or NULL where it can't be told.
char *os_exe_path(void);

// A process of the current user's, for picking one to attach a debugger to.
typedef struct
{
    long pid;
    char command[256]; // its command line (cut), or its program's name
} OsProcessInfo;

// The current user's processes but this one, by pid: a galloc'd array of
// *count entries, NULL (and *count 0) when they can't be listed.
OsProcessInfo *os_process_list(int *count);

/**
 * @brief Starts `argv` (argv[0] a path) as a background process of its own,
 * detached from the terminal: a new session on Unix, no console on
 * Windows. It doesn't end with this process or with the terminal, and its
 * stdin is the null device. Its stdout and stderr are appended to
 * `log_file`, or go to the null device when that is NULL.
 * @return 0 once it is started, or -1 with a one-line reason in `err`.
 */
int os_spawn_detached(const char *const *argv, const char *log_file, char *err, size_t err_size);

/***********************************************************************************************************************
 *
 *                                                 Time
 *
 **********************************************************************************************************************/

// Milliseconds on a clock that never jumps (for deadlines and debounce).
long long os_monotonic_ms(void);

void os_sleep_ms(int ms);

/***********************************************************************************************************************
 *
 *                                                 Streams and waiting
 *
 **********************************************************************************************************************/

// Something the editor reads from: the keyboard, a program's output, a
// terminal pane. Owned by whatever handed it out.
typedef struct OsStream OsStream;

/**
 * @brief Reads up to `n` bytes, waiting at most `timeout_ms` for the first
 * one (0: don't wait; -1: wait forever).
 * @return >0 bytes read; 0 nothing arrived in time; -1 end of stream
 * (the writer is gone, the program or shell has exited) or error.
 */
long os_stream_read(OsStream *s, void *buf, size_t n, int timeout_ms);

/**
 * @brief Waits until at least one of `streams` has data or has ended,
 * for at most `timeout_ms` (-1: forever). NULL entries are ignored.
 * @param ready Set to 1 for each stream os_stream_read() won't wait on,
 * 0 for the others.
 * @return How many are ready (0 on timeout).
 */
int os_wait(OsStream *const *streams, int n, int timeout_ms, int *ready);

/***********************************************************************************************************************
 *
 *                                                 The terminal the editor runs in
 *
 **********************************************************************************************************************/

/**
 * @brief Raw mode: keys delivered one at a time without echo, Ctrl-C as a
 * key rather than a signal, VT escape sequences interpreted on output.
 * @return 0, or -1 (not a terminal, or one too old for VT sequences).
 */
int os_term_enter_raw(void);

// Back to how the terminal was before os_term_enter_raw(). Safe to call
// more than once, or without having entered raw mode.
void os_term_leave_raw(void);

// The terminal's visible size in character cells. 0, or -1 if unknown.
int os_term_size(int *rows, int *cols);

// Writes all of `buf` to the terminal.
void os_term_write(const void *buf, size_t n);

// Keyboard input (see the conventions above).
OsStream *os_term_input(void);

/***********************************************************************************************************************
 *
 *                                                 Programs
 *
 **********************************************************************************************************************/

typedef struct OsProc OsProc;

typedef enum {
    OS_STDERR_DISCARD, // to the null device
    OS_STDERR_MERGE,   // into the same stream as stdout
    OS_STDERR_FILE     // appended to `stderr_file`
} OsStderrMode;

typedef struct {
    // Program and arguments, NULL-terminated. The program is looked up on
    // PATH (on Windows with PATHEXT too, so .cmd/.bat launchers such as
    // npm's run). Arguments reach it exactly as given: no shell involved.
    const char *const *argv;
    const char *cwd;            // NULL: the editor's working directory
    int stdin_pipe;             // 1: os_proc_write() feeds its stdin;
                                // 0: stdin is the null device
    OsStderrMode stderr_mode;
    const char *stderr_file;    // for OS_STDERR_FILE
    // 1: the program can't prompt for anything (no controlling terminal /
    // a hidden console, GIT_TERMINAL_PROMPT=0): a password prompt fails
    // at once instead of waiting for input that will never come.
    int no_prompt;
} OsSpawn;

/**
 * @brief Starts a program in the background, its stdout on a stream.
 * @details The program never shares the editor's terminal. A program that
 * can't be found still yields an OsProc, one that has already exited
 * with code 127 and produced no output — the same as on Unix, where that
 * is what the shell convention and execvp() failing in the child give.
 * @return NULL only if the program couldn't be started at all.
 */
OsProc *os_proc_spawn(const OsSpawn *spawn);

// The program's stdout (and stderr with OS_STDERR_MERGE).
OsStream *os_proc_output(OsProc *p);

// Writes to its stdin (OsSpawn.stdin_pipe). All of `buf`, or -1.
long os_proc_write(OsProc *p, const void *buf, size_t n);

// Ends the program and everything it started (a process group on Unix, a
// Job object on Windows).
void os_proc_kill(OsProc *p);

/**
 * @brief Waits up to `timeout_ms` (0: just check; -1: forever) for the
 * program to exit.
 * @param exit_code Its exit code, or -1 if it was killed by a signal or by
 * os_proc_kill().
 * @return 1 if it has exited, 0 if it's still running.
 */
int os_proc_wait(OsProc *p, int timeout_ms, int *exit_code);

// Releases `p`: closes its streams; a program still running is killed.
void os_proc_free(OsProc *p);

/**
 * @brief Where `name` would be found to run it: a path (one with a '/') is
 * checked as given, a bare name is looked up on PATH (on Windows with
 * PATHEXT too, the same lookup os_proc_spawn() does).
 * @details On Windows, a name not found on this process's PATH is looked
 * up again on the PATH in the registry (the user's and the machine's), and
 * when that finds it the process's PATH is updated: an installer such as
 * winget changes the registry, never an editor already running.
 * @return The full path (galloc'd), or NULL if it isn't installed there.
 */
char *os_find_program(const char *name);

/**
 * @brief The argv that runs `command` in the platform's shell: /bin/sh -c
 * on Unix, PowerShell on Windows (pwsh if installed). For os_proc_spawn();
 * release with os_free_argv().
 */
char **os_shell_argv(const char *command);
void os_free_argv(char **argv);

// `s` quoted as one literal word for that shell. galloc'd.
char *os_shell_quote(const char *s);

/***********************************************************************************************************************
 *
 *                                                 Terminal panes
 *
 **********************************************************************************************************************/

typedef struct OsPty OsPty;

/**
 * @brief Starts a shell attached to a new pseudo-terminal of rows×cols.
 * @param shell NULL: the user's shell ($SHELL, else zsh or /bin/sh on Unix;
 * $BELUGA_SHELL, then pwsh, then Windows PowerShell, then cmd.exe on
 * Windows).
 * @param cwd The directory it starts in; NULL: the editor's.
 */
OsPty *os_pty_open(const char *shell, const char *cwd, int rows, int cols);

/**
 * @brief Starts a program (not a shell) on a new pseudo-terminal: what a
 * debugger's runInTerminal asks for.
 * @param argv Program and arguments, NULL-terminated; the program is looked
 * up on PATH.
 * @param cwd NULL: the editor's directory.
 * @param env "NAME=value" to set, "NAME" alone to unset, on top of the
 * editor's environment; NULL-terminated, or NULL for none.
 * @return NULL if it couldn't be started.
 */
OsPty *os_pty_spawn(const char *const *argv, const char *cwd, const char *const *env,
                    int rows, int cols);

// The process id of what runs on it (the shell, or the program).
long os_pty_pid(OsPty *p);

// What the shell writes (VT escape sequences included); ends once it exits.
OsStream *os_pty_output(OsPty *p);

// Keys for the shell. All of `buf`, or -1.
long os_pty_write(OsPty *p, const void *buf, size_t n);

void os_pty_resize(OsPty *p, int rows, int cols);

// 1 while the shell is running.
int os_pty_alive(OsPty *p);

// Ends the shell and releases everything.
void os_pty_close(OsPty *p);

/***********************************************************************************************************************
 *
 *                                                 Local connections (daemon mode)
 *
 **********************************************************************************************************************/

// A byte stream between the beluga daemon and one of its clients, on this
// machine and for this user only: a Unix domain socket in a private
// directory on Unix, a named pipe that only the current user may open on
// Windows. `name` picks one of several daemons ("server" by default).
typedef struct OsServer OsServer;
typedef struct OsConn OsConn;

/**
 * @brief Starts listening for clients under `name`.
 * @return NULL, with a one-line reason in `err`, if it can't — including
 * when a daemon of that name is already running. (A socket left behind by
 * one that crashed is noticed and replaced.)
 */
OsServer *os_server_listen(const char *name, char *err, size_t err_size);

// Ready (for os_wait()) when a client is waiting to be accepted.
OsStream *os_server_stream(OsServer *s);

// The next waiting client, or NULL if none is.
OsConn *os_server_accept(OsServer *s);

// Stops listening; on Unix, also removes the socket.
void os_server_close(OsServer *s);

/**
 * @brief Connects to the daemon called `name`.
 * @return NULL, with a one-line reason in `err`, if none is running.
 */
OsConn *os_conn_connect(const char *name, char *err, size_t err_size);

/**
 * @brief Connects over TCP to `host` (a name or an address) on `port`,
 * waiting at most `timeout_ms` for the other end to answer. The
 * connection is used like a daemon's: os_conn_input(), os_conn_write(),
 * os_conn_close(). (Debug adapters that listen on a port: dlv dap,
 * codelldb, js-debug.)
 * @return NULL, with a one-line reason in `err`, if it can't.
 */
OsConn *os_tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t err_size);

// A TCP port on 127.0.0.1 nothing listens on right now (for an adapter to
// be told to listen on), or -1.
int os_tcp_free_port(void);

// What the other end sends; ends when it disconnects.
OsStream *os_conn_input(OsConn *c);

// Writes all of `buf`, or -1 (the other end is gone, or has stopped
// reading for several seconds).
long os_conn_write(OsConn *c, const void *buf, size_t n);

void os_conn_close(OsConn *c);

/***********************************************************************************************************************
 *
 *                                                 Files and directories
 *
 **********************************************************************************************************************/

typedef struct {
    char *name;
    int is_dir; // following symbolic links
} OsDirEntry;

/**
 * @brief The entries of a directory, "." and ".." excluded, in no
 * particular order. Release with os_free_dir().
 * @return NULL if `path` isn't a readable directory.
 */
OsDirEntry *os_list_dir(const char *path, int *count);
void os_free_dir(OsDirEntry *entries, int count);

typedef struct {
    int is_dir;               // following symbolic links
    int is_link;              // the path itself is a symbolic link
    long long size;
    long long mtime;          // seconds since the epoch
    char mode[11];            // as `ls -l` shows it: "drwxr-xr-x"
    char link_target[OS_PATH_MAX]; // for a link; "" otherwise
} OsFileInfo;

// 0, or -1 if `path` doesn't exist. Describes the path itself, not what a
// link points to (except is_dir).
int os_file_info(const char *path, OsFileInfo *info);

// The absolute, canonical path of an existing file (links resolved; on
// Windows, the case the file system stores). NULL if it doesn't exist.
char *os_realpath(const char *path);

char *os_getcwd(void);

// The user's home directory: $HOME, or %USERPROFILE% on Windows ("/" or
// "C:/" when unset). A galloc'd copy the caller gfree()s; NULL only when
// out of memory.
char *os_home_dir(void);

int os_path_is_absolute(const char *path);

// Length of a drive prefix ("C:") at the start of `path`: 2 on Windows,
// always 0 on Unix. Lexical path code keeps it as the root.
int os_path_drive_length(const char *path);

// A path typed the platform's way, converted in place to '/' separators:
// backslashes on Windows; nothing on Unix, where '\\' is an ordinary
// character in a file name.
void os_path_to_slashes(char *path);

// Do two paths name the same file, lexically? (Case-insensitively on
// Windows, where file names are.)
int os_path_equal(const char *a, const char *b);

/**
 * File operations behind dired. Each returns 0, or -1 with a one-line
 * reason in `err`.
 *  - delete: a file, or a directory with everything in it — permanently
 *    (rm -rf). A symbolic link is removed, never followed.
 *  - move / copy: like mv / cp -R: into `dest` when it is an existing
 *    directory, otherwise to `dest` itself (one source only). Copies keep
 *    symbolic links as links.
 *  - mkdirs: like mkdir -p.
 */
int os_delete_tree(const char *path, char *err, size_t err_size);
int os_move(const char *const *sources, int count, const char *dest, char *err, size_t err_size);
int os_copy_tree(const char *const *sources, int count, const char *dest, char *err, size_t err_size);
int os_mkdirs(const char *path, char *err, size_t err_size);

/**
 * Saving a file safely: write the new content to a temporary file next to
 * it, os_file_flush() it, then os_replace_file() it over the original.
 *  - flush: fflush plus fsync (Windows: _commit), so the data is on disk
 *    before the rename makes it visible. 0 or -1.
 *  - replace: gives `tmp` the permissions of `dest` when `dest` exists,
 *    then renames `tmp` to `dest` in one step, so a crash or a full disk
 *    leaves either the old file or the new one, never half of it. 0, or -1
 *    with a one-line reason in `err` (`tmp` is left for the caller to
 *    remove).
 */
int os_file_flush(FILE *f);
int os_replace_file(const char *tmp, const char *dest, char *err, size_t err_size);

/**
 * The C heap as the platform's allocator sees it: every block still in use,
 * galloc'd or not (libc's own, a library's, a raw malloc galloc never saw).
 * 0, or -1 where the platform can't tell. `blocks` is -1 when only the
 * byte count is known (glibc).
 */
int os_heap_in_use(long long *blocks, long long *bytes);

/***********************************************************************************************************************
 *
 *                                                 Clipboard
 *
 **********************************************************************************************************************/

// The system clipboard's text, with LF line endings; NULL if it holds no
// text (or there is no clipboard — on Unix, xclip/pbpaste missing).
char *os_clipboard_get(void);

// 0 or -1.
int os_clipboard_set(const char *text, size_t len);

#endif // BELUGA_OS_H_
