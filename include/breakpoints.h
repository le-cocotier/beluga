#ifndef BELUGA_BREAKPOINTS_H
#define BELUGA_BREAKPOINTS_H

#include "data.h"

// ─── Breakpoints ─────────────────────────────────────────────────────────────
// One editor-wide list, keyed by file path rather than by buffer or debug
// session: breakpoints can be set before a debugger runs, survive closing
// the file's buffer, and are sent again to every new session.
// Lines are 0-based, like buffer rows. They follow the edits made to an
// open buffer of their file (breakpointsLinesInserted/Removed, called by
// buffer.c), the way an IDE's breakpoints move with the code.

typedef struct
{
    char *path;      // absolute, as in buffer_t's fullname
    int line;        // 0-based
    char *condition; // NULL: always stops
    // NULL: every time. Otherwise how many hits it takes, in the adapter's
    // terms: "3" (the third hit on), ">= 5", "% 2" for gdb.
    char *hit_condition;
    // NULL: it stops. Otherwise a logpoint: the program doesn't stop, and
    // the message goes to the output, {expression}s replaced by their
    // values ("i = {i}").
    char *log_message;
    // What the debug adapter answered for it: 1 once it found code at that
    // line, 0 before a session has seen it (drawn hollow).
    int verified;
    int id; // the adapter's id for it, 0 for none yet
} Breakpoint;

// Adds a breakpoint at `line` of `path`, or removes the one already there.
// @return 1 if one was added, 0 if one was removed, -1 on failure.
int breakpointToggle(const char *path, int line);
// Sets (or, with NULL or "", clears) the condition of the breakpoint at
// `line` of `path`, adding the breakpoint if there is none. @return 0 or -1.
int breakpointSetCondition(const char *path, int line, const char *condition);
// Same for the hit count (Breakpoint.hit_condition) and the log message
// (Breakpoint.log_message, which makes it a logpoint).
int breakpointSetHitCondition(const char *path, int line, const char *hit_condition);
int breakpointSetLogMessage(const char *path, int line, const char *log_message);
// The breakpoint at `line` of `path`, or NULL.
Breakpoint *breakpointAt(const char *path, int line);
// Every breakpoint, sorted by path then line. *count gets how many.
Breakpoint *breakpointsAll(int *count);
// How many breakpoints `path` has.
int breakpointsInFile(const char *path);
// Removes every breakpoint. @return how many there were.
int breakpointsClear(void);
// Frees the list. Called on shutdown.
void breakpointsFree(void);

// Breakpoints of `buf`'s file at or after row `at` move down `count` rows:
// `count` rows were inserted before them.
void breakpointsLinesInserted(const struct buffer_t *buf, int at, int count);
// The breakpoint on row `from` of `buf`'s file moves to row `to` (unless
// `to` has one already): the row's code moved there, as when ENTER is
// pressed before it.
void breakpointsLineMoved(const struct buffer_t *buf, int from, int to);
// Rows [at, at + count) of `buf` were removed: the breakpoints after them
// move up, and the ones on them land on the row their text joined (at - 1).
void breakpointsLinesRemoved(const struct buffer_t *buf, int at, int count);

// `listener` is called with a file's path whenever the set of breakpoints
// of that file changes (added, removed, moved, condition): the debugger
// sends them again. NULL: nobody is told.
void breakpointsSetListener(void (*listener)(const char *path));

// Writes every breakpoint to `file` (replacing it in one step). 0 or -1.
int breakpointsSave(const char *file);
// Adds the breakpoints `file` holds, skipping those of files that no longer
// exist. Nobody is told. @return how many were added (0 without the file).
int breakpointsLoad(const char *file);
// Loads `file` and, from then on, saves to it whenever the list changes,
// so breakpoints outlive the editor. NULL stops saving.
void breakpointsSetStore(const char *file);

// The absolute path of `buf`'s file, galloc'd, or NULL when it isn't a file
// buffer.
char *breakpointsBufferPath(const struct buffer_t *buf);

#endif // BELUGA_BREAKPOINTS_H
