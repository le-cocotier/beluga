#ifndef OUTPUT_H_
#define OUTPUT_H_

#include "data.h"

/**
 * \fn void editorDrawRows(struct editorConfig *E, struct abuf *ab)
 * \brief Draws left rows of the editor.
 */

void editorRefreshScreen();
// Frees the last-frame cache editorRefreshScreen() keeps (teardown only).
void editorFreeScreenCache(void);

// The last frame written, so an identical one isn't written again.
typedef struct
{
    char *frame;
    int len;
    int cap;
    void *diff; // screen_diff.c's copy of what this window shows
} ScreenCache;

// Trades the current cache for `other` (daemon.c keeps one per client).
void editorSwapScreenCache(ScreenCache *other);

// Gutter geometry — the single source of truth for how many columns sit to
// the left of a pane's text (line numbers + diagnostic/git marker). Used by
// drawing, scrolling and cursor placement alike, plus the LSP completion
// popup's screen-x computation.
int editorLineNumberWidth(const struct buffer_t *buf);
int editorGutterWidth(const struct buffer_t *buf);

void editorScroll();

// At most this many enclosing definitions pinned atop a pane.
#define STICKY_MAX 3

// The rows sticky scroll pins atop `pane` (file rows, outermost first),
// as drawn: what a click on one of the pane's first rows lands on.
int editorPaneStickyRows(const EditorPane *pane, const struct buffer_t *buf, int *rows);

void editorDrawStatusBar(struct abuf *ab);

void editorDrawMessageBar(struct abuf *ab);

#endif // OUTPUT_H_
