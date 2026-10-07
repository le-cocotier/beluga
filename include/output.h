#ifndef OUTPUT_H_
#define OUTPUT_H_

#include "data.h"

/**
 * \fn void editorDrawRows(struct editorConfig *E, struct abuf *ab)
 * \brief Draws left rows of the editor.
 */

void editorRefreshScreen();

// Gutter geometry — the single source of truth for how many columns sit to
// the left of a pane's text (line numbers + diagnostic/git marker). Used by
// drawing, scrolling and cursor placement alike, plus the LSP completion
// popup's screen-x computation.
int editorLineNumberWidth(const struct buffer_t *buf);
int editorGutterWidth(const struct buffer_t *buf);

void editorScroll();

void editorDrawStatusBar(struct abuf *ab);

void editorDrawMessageBar(struct abuf *ab);

#endif // OUTPUT_H_
