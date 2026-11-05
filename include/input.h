#ifndef INPUT_H_
#define INPUT_H_

#include "data.h"
#include "define.h"
#include "output.h"
#include "terminal.h"
#include "builtins.h"

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

Utf8Char *editorPrompt(const char *prompt, Utf8Char *placeHolder, char bPathMode);
// Same, but ENTER on an empty input returns "" instead of being ignored.
Utf8Char *editorPromptEx(const char *prompt, Utf8Char *placeHolder, char bPathMode,
                         int allowEmpty);

/**
 * @brief The matches for TAB completion of a path in a prompt
 * (see fileCompletionMatches()).
 */
typedef struct {
  char *dir;     /**< the path up to and including its last '/' */
  char **names;  /**< sorted matching names, directories ending in '/' */
  int count;
  size_t common; /**< length of the prefix all names share */
} FileCompletion;

int fileCompletionMatches(const char *path, FileCompletion *fc);
void fileCompletionFree(FileCompletion *fc);

int editorMoveCursor(int key);

int executeKeyBind(char *key_sequence);
int keyBindScope(const struct keyBind_t *kb, const struct buffer_t *buf);

// Scrolls the active pane's view by `lines` (negative: up); the cursor
// only moves if it would otherwise leave the pane.
void editorScrollView(int lines);

// Focuses the pane under the last mouse report (E.mouse_row/mouse_col).
// Returns 0 if the pointer wasn't over a pane.
int editorMouseFocusPane(void);

// Focuses the pane under the last mouse report and moves the cursor to
// the character there.
void editorMouseSetCursor(void);

/**
 * \fn void editorProcessKeypress()
 * \brief Get the last key input and do the proper action.
 */

void editorProcessKeypress();

#endif // INPUT_H_
