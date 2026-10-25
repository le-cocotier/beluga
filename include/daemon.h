/**
 * @file daemon.h
 * @brief Daemon mode: one beluga process serving several terminal windows.
 * @details `beluga --daemon` keeps the buffers, the Lisp interpreter, the
 * language servers and the terminal panes; `beluga -c [file]`, run in any
 * terminal, opens a window on it. Each window has its own layout, popups
 * and status line; the text is shared. See daemon.c.
 */
#ifndef DAEMON_H_
#define DAEMON_H_

// The daemon name used when none is given (--daemon=NAME, --name=NAME).
#define DAEMON_DEFAULT_NAME "server"

/**
 * @brief Runs the daemon until it is stopped, in the foreground
 * (--foreground; clientStartDaemon() runs it that way in the background). The editor is initialised
 * here; the caller tears it down (deInitEditor(), bFree_structs()) after a
 * return of 0.
 * @return 0, or 1 if it couldn't start (already running, no socket).
 */
int daemonMain(const char *name);

// 1 inside a running daemon: CTRL-x CTRL-c then closes the window it was
// typed in, not the editor.
int daemonIsRunning(void);

/**
 * @brief Starts the daemon called `name` in the background (detached from
 * the terminal, its output in <beluga dir>/daemon-NAME.log) and waits
 * until it takes connections. `beluga --daemon` without --foreground, and
 * `beluga -c` when none is running.
 * @return 0, or 1 with the reason on stderr.
 */
int clientStartDaemon(const char *name);

/**
 * @brief Runs a client window (beluga -c) on the daemon called `name`,
 * opening `files` (relative to the current directory) in it.
 * @return The process exit code.
 */
int clientMain(const char *name, char *const *files, int n_files);

/**
 * @brief beluga --kill: stops the daemon called `name`. It refuses while a
 * buffer has unsaved changes, unless `force`.
 * @return The process exit code.
 */
int clientKill(const char *name, int force);

#endif // DAEMON_H_
