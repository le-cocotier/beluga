#ifndef BELUGA_DEBUG_UI_H
#define BELUGA_DEBUG_UI_H

#include "data.h"

// ─── The debugger in the editor's panes ──────────────────────────────────────
// Gutter marks (breakpoints, the line the program is stopped on), the
// "*debug*" panel (state, call stack, variables, the end of the output)
// and the "*debug-output*" buffer. Both buffers are read-only scratch
// buffers re-rendered from dap.c's state whenever it changes.

// The path of `buf`'s file, galloc'd, when its gutter has debugger marks to
// draw (breakpoints, or the program stopped in it); NULL otherwise.
// editorDrawPane() gets it once per pane and hands it to
// debugUiDrawGutter() / debugUiIsCurrentLine() for each row.
char *debugUiGutterPath(const struct buffer_t *buf);

// Draws the marker column (the mark and its padding, GUTTER_WIDTH columns)
// for row `line` of `path` when it has a debugger mark. @return 1 if it
// drew one, 0 if the caller draws the usual diagnostic/git mark.
int debugUiDrawGutter(struct abuf *ab, const char *path, int line);

// Is row `line` of `path` where the program is stopped (the selected
// frame)? It's drawn on the theme's "debug-line-background".
int debugUiIsCurrentLine(const char *path, int line);

// While the program is paused in `path`: after row `line` of `buf` (the
// stopped line or one of the rows above it), the values of the variables
// it names, "  total = 3, i = 1", in at most `max_cols` columns.
// @return the columns drawn (0: nothing to show). DEBUG-INLINE-VALUES #f
// turns it off.
int debugUiDrawInlineValues(struct abuf *ab, const char *path, const struct buffer_t *buf,
                            int line, int max_cols);

// Does `buf` need the marker column for debugger marks alone?
int debugUiWantsGutter(const struct buffer_t *buf);

// Shows the "*debug*" panel: in a pane of its own, splitting the screen
// when there's only one, focus staying where it was.
void debugUiOpenPanel(void);
// Re-renders the panel and the output buffer, if they exist.
void debugUiRefresh(void);
// The index of the watch expression on the cursor's row when the panel is
// the active pane, else -1.
int debugUiPanelWatch(void);
// Shows row `line` (0-based) of `path` in a pane that isn't one of the
// debugger's own buffers.
void debugUiShowLocation(const char *path, int line);
// ENTER in the panel: selects the frame or opens/closes the variable on
// the cursor's row.
void debugUiPanelSelect(void);
// Shows "*debug-output*", everything the program and the adapter wrote,
// in the active pane.
void debugUiShowOutput(void);

#endif // BELUGA_DEBUG_UI_H
