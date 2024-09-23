#ifndef INPUT_H_
#define INPUT_H_

#include "data.h"
#include "define.h"
#include "terminal.h"
#include <unistd.h>

void editorMoveCursor(struct editorConfig *E, int key);

/**
 * \fn void editorProcessKeypress()
 * \brief Get the last key input and do the proper action.
 */

void editorProcessKeypress(struct editorConfig *E);

#endif // INPUT_H_
