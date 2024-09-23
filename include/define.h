#ifndef DEFINE_H_
#define DEFINE_H_

#define CTRL_KEY(k) ((k) & 0x1f)

#define CURSOR_TOP_LEFT "\x1b[H"
#define HIDE_CURSOR "\x1b[?25l"
#define SHOW_CURSOR "\x1b[?25h"
#define ERASE_END_LINE "\x1b[K"

enum editorKey {
  CURSOR_LEFT = 1000,
  CURSOR_RIGHT,
  CURSOR_UP,
  CURSOR_DOWN,
  BEG_LINE,
  DELETE,
  END_LINE,
  PAGE_UP,
  PAGE_DOWN,
};

#define ABUF_INIT {NULL, 0}

#define BELUGA_VERSION "0.1"

#endif // DEFINE_H_
