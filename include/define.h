#ifndef DEFINE_H_
#define DEFINE_H_

#define CTRL_KEY(k) ((k) & 0x1f)

#define ESCAPE '\x1b'
#define CURSOR_TOP_LEFT "\x1b[H"
#define HIDE_CURSOR "\x1b[?25l"
#define SHOW_CURSOR "\x1b[?25h"
#define ERASE_END_LINE "\x1b[K"

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  BEG_LINE,
  END_LINE,
  PAGE_UP,
  PAGE_DOWN,
};

#define ABUF_INIT {NULL, 0}

#define BELUGA_VERSION "1.0"
#define TAB_LENGTH 4
#define QUIT_TIMES 1

#endif // DEFINE_H_
