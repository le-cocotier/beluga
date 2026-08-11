#ifndef OUTPUT_H_
#define OUTPUT_H_

#include "data.h"
#include <unistd.h>

/**
 * \fn void editorDrawRows(struct editorConfig *E, struct abuf *ab)
 * \brief Draws left rows of the editor.
 */

void editorRefreshScreen();

void editorScroll();

void editorDrawStatusBar(struct abuf *ab);

void editorDrawMessageBar(struct abuf *ab);

#endif // OUTPUT_H_
