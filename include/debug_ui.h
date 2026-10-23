#ifndef BELUGA_DEBUG_UI_H
#define BELUGA_DEBUG_UI_H

#include "data.h"

// ─── The debugger in the editor's panes ──────────────────────────────────────
// Gutter marks for breakpoints, drawn in the marker column in place of a
// diagnostic or git mark.

// The path of `buf`'s file, galloc'd, when its gutter has debugger marks to
// draw (it has breakpoints); NULL otherwise. editorDrawPane() gets it once
// per pane and hands it to debugUiDrawGutter() for each row.
char *debugUiGutterPath(const struct buffer_t *buf);

// Draws the marker column (the mark and its padding, GUTTER_WIDTH columns)
// for row `line` of `path` when it has a debugger mark. @return 1 if it
// drew one, 0 if the caller draws the usual diagnostic/git mark.
int debugUiDrawGutter(struct abuf *ab, const char *path, int line);

// Does `buf` need the marker column for debugger marks alone?
int debugUiWantsGutter(const struct buffer_t *buf);

#endif // BELUGA_DEBUG_UI_H
