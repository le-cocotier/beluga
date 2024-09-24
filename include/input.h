#ifndef INPUT_H_
#define INPUT_H_

#include "data.h"
#include "define.h"
#include "terminal.h"
#include <unistd.h>

// KEYS keycode
//
// ARROW_UP \x1b[A
// ARROW_DOWN \x1b[B
// ARROW_RIGHT \x1b[C
// ARROW_LEFT \x1b[D
// PAGE_UP \x1b[5~
// PAGE_DOWN \x1b[6~
// HOME \x1b[1~ || <esc>[7~ || <esc>[H  || <esc>OH
// END \x1b[4~ || <esc>[8~ || <esc>[F  || <esc>OF
// DELETE \x1b[3~

void editorMoveCursor(struct editorConfig *E, int key);

/**
 * \fn void editorProcessKeypress()
 * \brief Get the last key input and do the proper action.
 */

void editorProcessKeypress(struct editorConfig *E);

#endif // INPUT_H_
