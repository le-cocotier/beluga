#ifndef BELUGA_DAP_H
#define BELUGA_DAP_H

#include <stddef.h>

#include "os.h"

// ─── The debugger: a Debug Adapter Protocol client ───────────────────────────
// One debug session per project (its root, or the working directory
// outside any), talking to a debug adapter (gdb -i dap, lldb-dap,
// debugpy, ...) over its stdin/stdout, or a TCP connection, with the same
// framing as LSP. The commands and what the panel shows are the current
// project's session. Which adapter a file type uses is set from Lisp, like lsp-server:
//   (dap-adapter "c" "gdb" "-i" "dap")
// The launch arguments are handed to the adapter as they are (a JSON
// object), so beluga needs no knowledge of each adapter's own options.
//
// Everything here is asynchronous: requests go out, and the answers and
// events are handled from the run loop (dapPoll()), which updates the
// panel (debug_ui.c), the gutter's current-line mark and the hooks
// debug-stopped / debug-exited.

typedef enum
{
    DAP_OFF = 0,  // no session
    DAP_STARTING, // adapter started, program not yet running
    DAP_RUNNING,  // the program runs
    DAP_STOPPED,  // the program is paused (breakpoint, step, pause, ...)
} DapState;

// One frame of the paused thread's call stack.
typedef struct
{
    int id;     // the adapter's frameId
    char *name; // function
    char *path; // source file, NULL when the adapter has none (no debug info)
    int line;   // 0-based
} DapFrame;

// A scope (Locals, Arguments, Registers, ...) or a variable in one: a node
// of the variables tree the panel shows. `ref` > 0 means it has children,
// fetched the first time it is expanded.
typedef struct dap_var_t
{
    char *name;
    char *value; // NULL for a scope
    char *type;  // NULL when the adapter doesn't say
    int ref;     // variablesReference
    int expanded;
    int loaded;  // children fetched
    struct dap_var_t *children;
    int child_count;
} DapVar;

// ── Adapters ────────────────────────────────────────────────────────────────
// Registers the adapter command for files with extension `ext` (the first
// registration for an extension wins, like lsp-server). argv is copied.
// launch_defaults (a JSON object, or NULL) fills in the launch arguments
// a session doesn't give itself.
// @return 0, or -1 when the table is full.
int dapRegisterAdapter(const char *ext, const char *const *argv, const char *launch_defaults);

// ── Session ─────────────────────────────────────────────────────────────────
// Starts the adapter registered for `ext` (NULL or "": the active buffer's
// extension, else the first registered adapter) and launches the program
// with `launch_json`, the adapter's launch arguments as a JSON object
// ("program", "args", "cwd", ...; "cwd" defaults to the project root).
// @return 0, or -1 (with a status message) if it couldn't start.
int dapStart(const char *ext, const char *launch_json);
// The same for a program already running: `attach_json` is the adapter's
// attach arguments ("pid" for gdb and lldb-dap, "processId" for debugpy).
// Stopping the session detaches, leaving the program running.
int dapAttach(const char *ext, const char *attach_json);
// Ends the session: terminates the program and the adapter.
void dapStop(void);
void dapContinue(void);
void dapNext(void);
void dapStepIn(void);
void dapStepOut(void);
void dapPause(void);
// Evaluates `expression` in the selected frame. repl: as typed in the
// adapter's console (for gdb, a gdb command such as "info registers");
// otherwise as an expression. The result goes to the output.
void dapEvaluate(const char *expression, int repl);
// Exception breakpoints: which exceptions stop the program, by the
// adapter's filter ids (debugpy: "raised", "uncaught"; gdb: "throw",
// "catch"). `ids` is a space-separated list ("" for none, NULL for each
// adapter's defaults), sent to every session and to the next ones.
void dapSetExceptionFilters(const char *ids);
// The filters the current project's adapter offers, as "id (label), ..."
// with a "*" before the ones on; NULL (and *count 0) without a session.
// galloc'd.
char *dapExceptionFilters(int *count);
// The ids on now, space-separated, galloc'd.
char *dapExceptionChoice(void);
// Watch expressions: evaluated in the selected frame whenever the program
// stops, for every session. Adding one already there does nothing.
// @return 0, or -1 (empty, out of memory, no such index).
int dapWatchAdd(const char *expression);
int dapWatchRemove(int index);
const char *const *dapWatches(int *count);
// What watch `index` came to in the current project's paused program, or
// NULL (not stopped, not answered yet).
const char *dapWatchValue(int index);
// Selects frame `index` of the call stack: its source is shown and its
// variables listed.
void dapSelectFrame(int index);
// Expands or collapses a node of the variables tree.
void dapToggleVar(DapVar *var);

DapState dapState(void);
// What the panel shows. Valid until the next dapPoll()/command.
const DapFrame *dapFrames(int *count, int *selected);
DapVar *dapScopes(int *count);
// Why the program stopped ("breakpoint", "step", ...), or "".
const char *dapStopReason(void);
// The adapter's command, for the panel's title ("" without a session).
const char *dapAdapterName(void);
// The program's exit code once it has exited, else -1.
int dapExitCode(void);
// The line the selected frame is on: 1 with *path/*line set, 0 when the
// program isn't stopped in a known source line.
int dapCurrentLine(const char **path, int *line);
// The last lines of output (program and adapter), oldest first.
const char *const *dapOutputTail(int *count);

// ── Run loop ────────────────────────────────────────────────────────────────
// The adapters' output streams (one per running session), into streams[0..max).
// @return how many.
int dapStreams(OsStream **streams, int max);
// Reads what the adapters sent and handles every complete message.
void dapPoll(void);
// Feeds bytes as if the current project's adapter had sent them (tests).
void dapFeed(const char *data, size_t len);
// Time-driven work (sending launch when the adapter didn't ask first,
// disconnecting after the program ended), once per run loop tick.
void dapTick(void);
// Ends every session and frees the adapter table. Called on shutdown.
void dapFree(void);

// Daemon mode: a session reports to the window it was started from.
// `current` names the window acting now (0: none); `enter` makes window
// `id` current while a session's messages are handled (0 if it is gone:
// they are handled where the daemon is) and `leave` goes back.
void dapSetWindowHooks(int (*current)(void), int (*enter)(int id), void (*leave)(void));

#endif // BELUGA_DAP_H
