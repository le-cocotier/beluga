#ifndef OUTPUT_H_
#define OUTPUT_H_

#include "append_buffer.h"
#include "data.h"
#include "define.h"
#include <stdio.h>
#include <unistd.h>

/**
 * \fn void editorDrawRows(struct editorConfig *E, struct abuf *ab)
 * \brief Draws left rows of the editor.
 */

void editorDrawRows(struct editorConfig *E, struct abuf *ab);

void editorRefreshScreen(struct editorConfig *E);

#endif // OUTPUT_H_
