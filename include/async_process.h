#ifndef BELUGA_ASYNC_PROCESS_H
#define BELUGA_ASYNC_PROCESS_H

#include <sys/select.h>

#include "lisp.h"

// ─── Background processes ────────────────────────────────────────────────────
// Programs run without blocking the editor — a `git push` or `fetch` over
// the network can take seconds, far too long for process-run's synchronous
// wait. editorRunLoop() adds their output pipes to its select(), reads them
// as data arrives, and once a program exits calls its Lisp callback with
// (exit-code . "output"), exactly the shape process-run returns.

// Starts argv[0] (searched on PATH, no shell) in the background with
// stdout+stderr captured and stdin from /dev/null, in a session of its own
// — so it has no controlling terminal: a password or passphrase prompt
// fails instead of drawing over the editor. `callback` is called once, from
// the main loop, when the program exits.
// @return an id >= 0, or -1 if it couldn't be started (too many running,
// fork/pipe failure).
int asyncProcessStart(char *const argv[], Lisp callback);

// Adds every running process's pipe to `set`; returns the new max fd.
int asyncProcessAddFds(fd_set *set, int maxfd);

// Reads whatever is ready in `set`, and finishes (reaps + calls back) every
// process whose output has reached EOF. Call after select().
void asyncProcessPoll(const fd_set *set);

// Number of processes still running — shown in the status bar by callers
// that want to say "working…".
int asyncProcessCount(void);

#endif // BELUGA_ASYNC_PROCESS_H
