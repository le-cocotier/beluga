#ifndef SUBPROCESS_H_
#define SUBPROCESS_H_

#include <stddef.h>

// ─── Bounded command execution ──────────────────────────────────────────────
// One hardened "run a shell command, capture its stdout" helper, shared by
// the git integration (branch/diff status) and project-wide search. Both
// run synchronously on the editor's own thread, so neither can be allowed
// to block on a slow or hung child — hence the deadline and the output cap
// rather than a plain popen().

// Runs `cmd` via /bin/sh, returning its stdout malloc'd and NUL-terminated
// with trailing newlines stripped, or NULL if it produced no output at all
// (which is also how a failed command reports itself — redirect the child's
// stderr inside `cmd` with "2>/dev/null" if you don't want it on the
// terminal).
//
// timeout_ms bounds the whole call: when the deadline passes, the child's
// entire process group is killed and whatever was read so far is returned
// (NULL if nothing was). max_bytes caps how much output is accumulated —
// reading stops there and the child is killed, so a runaway command can't
// grow the buffer without bound. Pass 0 for no cap.
char *runCommandCapture(const char *cmd, long timeout_ms, size_t max_bytes);

// Wraps `s` in single quotes for safe use inside a shell command string,
// escaping any embedded single quotes. Always NUL-terminates, and truncates
// rather than overflowing if `out_size` is too small.
void shellQuote(const char *s, char *out, size_t out_size);

#endif
