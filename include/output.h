#ifndef OUTPUT_H_
#define OUTPUT_H_

#include "append_buffer.h"
#include "data.h"
#include "define.h"
#include "row_op.h"
#include <stdio.h>
#include <unistd.h>

/**
 * \fn void editorDrawRows(struct editorConfig *E, struct abuf *ab)
 * \brief Draws left rows of the editor.
 */

void editorDrawRows(struct abuf *ab);

void editorRefreshScreen();

void editorScroll();

void editorDrawStatusBar(struct abuf *ab);

void editorDrawMessageBar(struct abuf *ab);

void editorSetStatusMessage(const char *fmt, ...);

#endif // OUTPUT_H_
