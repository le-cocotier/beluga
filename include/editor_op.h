#ifndef EDITOR_OP_H_
#define EDITOR_OP_H_

#include "data.h"
#include "define.h"
#include "os.h"

void editorSetStatusMessage(const char *fmt, ...);
// The working directory (E.cwd, else the process's), galloc'd; NULL only
// when it can't be told.
char *editorGetCwd(void);
int editorRowCharCount(Row *row, int x);
int editorRowCxToByte(const Row *row, int cursor_x);
char *editorGetClipboard(void);
void editorSetClipboard(const char *text, int len);
void editorRunLoop(void);

// ── Pieces of the run loop, shared with the daemon's (daemon.c) ─────────────

// Upper bound on the streams a run loop waits on: the keyboard (or the
// daemon's clients), one per terminal pane, the language server,
// background processes.
#define RUN_LOOP_MAX_STREAMS 64
#define RUN_LOOP_MAX_TERMINALS 32

// What editorWatchStreams() added to a wait, for editorHandleStreams().
typedef struct
{
    struct buffer_t *terminals[RUN_LOOP_MAX_TERMINALS];
    int n_terminals;
    int first_terminal; // index of terminals[0]'s stream
    // Index of each LSP client's output stream, -1 when it has none.
    int lsp_index[LSP_MAX_CLIENTS];
    // The debug adapters' output streams: dap_count of them from dap_first.
    int dap_first;
    int dap_count;
} RunLoopWatch;

// Appends the terminal panes, the language server and background
// processes to streams[*n_streams..max), advancing *n_streams.
void editorWatchStreams(RunLoopWatch *w, OsStream **streams, int *n_streams, int max);
// Reads whatever those streams have, after os_wait() filled `ready`.
void editorHandleStreams(const RunLoopWatch *w, const int *ready);
// Time-driven work (LSP debounces, autosave, files changed on disk),
// once per tick.
void editorTick(void);
// Ends every terminal pane's shell (on the way out).
void editorCloseTerminals(void);
// The window is now rows x cols: resizes the panes and their shells.
void editorApplyScreenSize(int rows, int cols);


#endif // EDITOR_OP_H_
