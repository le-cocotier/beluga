#include "../include/input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * \fn char * editorPrompt(struct editorConfig *E, char *prompt)
 * \brief Return user input in a prompt when enter is hit. */

char *key_to_string(int key) {
  static char key_str[32];

  char * tmp = malloc(10 * sizeof(char));
  sprintf(tmp, "%d\n", key);
  log_string(tmp);


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

char *editorPrompt(struct editorConfig *E, char *prompt) {
  size_t buf_size = 128;
  char *buf = malloc(buf_size);
  size_t buf_len = 0;
  int c = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(E, prompt, buf);
    editorRefreshScreen(E);
    c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buf_len != 0) {
        buf[--buf_len] = '\0';
      }
    } else if (c == ESCAPE) {
      fprintf(stderr, "escape");
      editorSetStatusMessage(E, "");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buf_len != 0) {
        editorSetStatusMessage(E, "");
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

void editorMoveCursor(struct editorConfig *E, int key) {
  erow *row = (E->cursor_y >= E->numrows) ? NULL : &E->row[E->cursor_y];
  int row_len;
  char *sequence = key_to_string(key);
  switch (key) {
  case ARROW_RIGHT:
    if (row && E->cursor_x < row->size) {
      ++E->cursor_x;
    } else if (row && E->cursor_x == row->size) {
      E->cursor_y++;
      E->cursor_x = 0;
    }
    break;
  case ARROW_DOWN:
    if (E->cursor_y < E->numrows) {
      ++E->cursor_y;
    }
    break;
  case ARROW_UP:
    if (E->cursor_y != 0) {
      --E->cursor_y;
    }
    break;
  case ARROW_LEFT:
    if (E->cursor_x != 0) {
      --E->cursor_x;
    } else if (E->cursor_y > 0) {
      --E->cursor_y;
      E->cursor_x = E->row[E->cursor_y].size;
    }
    break;
  }

  row = (E->cursor_y >= E->numrows) ? NULL : &E->row[E->cursor_y];
  row_len = row ? row->size : 0;
  if (E->cursor_x > row_len) {
    E->cursor_x = row_len;
  }
}

key_sequence_t current_sequence = {0};

int handle_key_sequence(struct editorConfig *E, int key) {
  char *key_str = key_to_string(key);

  log_string(key_str);

  // Add current key to sequence
  if (current_sequence.sequence_len > 0) {
    strcat(current_sequence.sequence, " ");
  }
  strcat(current_sequence.sequence, key_str);
  current_sequence.sequence_len++;

  // Check if this sequence matches any binding
  const char *command =
      config_get_key_mapping(E->config, current_sequence.sequence);
  if (command) {
    log_string("Command found\n");
    // Found a complete binding - execute it
    execute_key_binding(E->config, current_sequence.sequence, E);

    // Reset sequence
    memset(&current_sequence, 0, sizeof(current_sequence));
    return 1; // Handled
  }

  // Check if this could be the start of a longer sequence
  // (This is a simple check - you might want to make it more sophisticated)
  int potential_match = 0;
  // You'd implement a function to check for partial matches here

  if (!potential_match) {
    // No potential matches, reset sequence and handle as single key
    memset(&current_sequence, 0, sizeof(current_sequence));
    return 0; // Not handled
  }

  return 1; // Waiting for more keys in sequence
}

int execute_key_binding(config_t *config, const char *key_combo,
                        void *context) {
  const char *command = config_get_key_mapping(config, key_combo);
  if (!command) {
    log_string("No mapping found for key combination: ");
    log_string(key_combo);
    log_string("\n");
    return -1;
  }

  // Remove the '%' prefix if present
  const char *func_name = command;
  if (command[0] == '%') {
    func_name = command + 1;
  }

  return execute_command(func_name, context);
}

void editorProcessKeypress(struct editorConfig *E) {
  static int quit_times = QUIT_TIMES;
  int c = editorReadKey();

  if (E->config) {
    if (handle_key_sequence(E, c)) {
      quit_times = QUIT_TIMES;
      return; // Key was handled by config system
    }
  }

  editorInsertChar(E, c);
  // reset quit times
  E->quit_times = QUIT_TIMES;
}
