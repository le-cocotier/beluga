#ifndef BELUGA_ASYNC_PROCESS_H
#define BELUGA_ASYNC_PROCESS_H

#include "lisp.h"
#include "os.h"

// ─── Background processes ────────────────────────────────────────────────────
// Programs run without blocking the editor — a `git push` or `fetch` over
// the network can take seconds, far too long for process-run's synchronous
// wait. editorRunLoop() waits on their output streams, reads them as data
// arrives, and once a program exits calls its Lisp callback with
// (exit-code . "output"), exactly the shape process-run returns.

// Starts argv[0] (searched on PATH, no shell) in the background with
// stdout+stderr captured and stdin empty, unable to prompt: a password or passphrase prompt
// fails instead of drawing over the editor. `callback` is called once, from
// the main loop, when the program exits.
// @return an id >= 0, or -1 if it couldn't be started (too many running,
// couldn't start).
int asyncProcessStart(char *const argv[], Lisp callback);

// The output streams of every running process, for editorRunLoop() to wait
// on (up to `max`); returns how many.
int asyncProcessStreams(OsStream **streams, int max);

// Reads whatever output is ready, and finishes (reaps + calls back) every
// process whose output has reached its end. Call once per loop iteration.
void asyncProcessPoll(void);

// Number of processes still running — shown in the status bar by callers
// that want to say "working…".
int asyncProcessCount(void);

#endif // BELUGA_ASYNC_PROCESS_H
