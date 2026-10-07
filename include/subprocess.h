#ifndef SUBPROCESS_H_
#define SUBPROCESS_H_

#include <stddef.h>

// ─── Bounded program execution ──────────────────────────────────────────────
// Run a program and capture its output, synchronously, on the editor's own
// thread — behind process-run, shell-run, the git integration, project
// search and the file finder. Hence the deadline and the output cap rather
// than a plain "run and read until done".
//
// Programs are started from argv (looked up on PATH; no shell involved, so
// arguments need no quoting at all), with stdin empty and no way to prompt.
//
// Each returns the output galloc'd and NUL-terminated with trailing
// newlines stripped, or NULL if there was none. timeout_ms bounds the
// whole call: when it passes, the program and everything it started are
// killed and whatever was read so far is returned. max_bytes caps how much
// output is kept (0: no cap). *exit_status (if non-NULL) gets the exit
// code, 127 if the program couldn't be found, or -1 if it was killed.

// stdout and stderr together.
char *runArgvCapture(char *const argv[], long timeout_ms, size_t max_bytes,
                     int *exit_status);

// Same, run from directory `cwd` (NULL: the editor's own).
char *runArgvCaptureIn(char *const argv[], const char *cwd, long timeout_ms,
                       size_t max_bytes, int *exit_status);

// stdout only; stderr is discarded (git asked something on a path that
// isn't in a repository, say).
char *runArgvCaptureQuiet(char *const argv[], long timeout_ms, size_t max_bytes,
                          int *exit_status);

// runArgvCaptureQuiet(), for output that may contain NUL bytes (git's -z
// format): *len gets its length.
char *runArgvCaptureBytes(char *const argv[], long timeout_ms, size_t max_bytes,
                          size_t *len);

#endif
