#include "../include/input.h"
#include "../include/editor_op.h"
#include "../include/output.h"
#include "../include/define.h"
#include "data.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern struct editorConfig E;

/**
 * \fn char * editorPrompt(struct editorConfig *E, char *prompt)
 * \brief Return user input in a prompt when enter is hit. */

char *editorPrompt(char *prompt) {
  size_t buf_size = 128;
  char *buf = malloc(buf_size);
  size_t buf_len = 0;
  int c = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buf_len != 0) {
        buf[--buf_len] = '\0';
      }
    } else if (c == ESCAPE) {
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buf_len != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buf_len == buf_size - 1) {
        buf_size *= 2;
        buf = realloc(buf, buf_size);
      }
      buf[buf_len++] = c;
      buf[buf_len] = '\0';
    }
  }
}

char *key_to_string(int key) {
  static char key_str[32];

  char tmp[10];
  sprintf(tmp, "%d", key);


  // First test enter key

  if (key == '\r') {
    strcpy(key_str, "ENTER");
  } else if (key >= 1 && key <= 26) { // CTRL keys
    snprintf(key_str, sizeof(key_str), "CTRL-%c", 'a' + key - 1);
  } else {
    switch (key) {
    case ARROW_UP:
      strcpy(key_str, "ARROW-UP");
      break;
    case ARROW_DOWN:
      strcpy(key_str, "ARROW-DOWN");
      break;
    case ARROW_LEFT:
      strcpy(key_str, "ARROW-LEFT");
      break;
    case ARROW_RIGHT:
      strcpy(key_str, "ARROW-RIGHT");
      break;
    case PAGE_UP:
      strcpy(key_str, "PAGE-UP");
      break;
    case PAGE_DOWN:
      strcpy(key_str, "PAGE-DOWN");
      break;
    case DEL_KEY:
      strcpy(key_str, "DEL");
      break;
    case BACKSPACE:
      strcpy(key_str, "BACKSPACE");
      break;
    case '\r':
      strcpy(key_str, "ENTER");
      break;
    case '\x1b':
      strcpy(key_str, "ESCAPE");
      break;
    case BEG_LINE:
      strcpy(key_str, "HOME");
      break;
    case END_LINE:
      strcpy(key_str, "END");
      break;
    default:
      // For regular characters
      if (isprint(key)) {
        snprintf(key_str, sizeof(key_str), "%c", key);
      } else {
        snprintf(key_str, sizeof(key_str), "KEY-%d", key);
      }
    }
  }
  return key_str;
}


void editorMoveCursor(int key) {
  erow *row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  int row_len;
  switch (key) {
  case ARROW_RIGHT:
    if (row && E.cursor_x < row->size) {
      ++E.cursor_x;
    } else if (row && E.cursor_x == row->size) {
      E.cursor_y++;
      E.cursor_x = 0;
    }
    break;
  case ARROW_DOWN:
    if (E.cursor_y < E.numrows) {
      ++E.cursor_y;
    }
    break;
  case ARROW_UP:
    if (E.cursor_y != 0) {
      --E.cursor_y;
    }
    break;
  case ARROW_LEFT:
    if (E.cursor_x != 0) {
      --E.cursor_x;
    } else if (E.cursor_y > 0) {
      --E.cursor_y;
      E.cursor_x = E.row[E.cursor_y].size;
    }
    break;
  }

  row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  row_len = row ? row->size : 0;
  if (E.cursor_x > row_len) {
    E.cursor_x = row_len;
  }
}

int executeKeyBind(char *key_sequence) {
  int i;
  for (i = 0; i < E.number_of_keybinds; ++i) {
    if (!strcmp(key_sequence, E.key_binds[i].key_sequence)) {
  
        fprintf(stderr, "lisp function %s\n", key_sequence);
        // It's a symbol, create a function call
        lisp_eval(lisp_cons(E.key_binds[i].command, lisp_null(), E.ctx),
                  &E.ctx_error, E.ctx);
        return 1;
    }
  }
  return 0;
}

void editorProcessKeypress() {
  int c = editorReadKey();

  if (executeKeyBind(key_to_string(c))) {
    return;
  }
  editorInsertChar(c);
  E.quit_times_buffer = E.constantes.QUIT_TIMES;

}
