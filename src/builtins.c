
// function_registry.c
#include "../include/builtins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Static array to hold function mappings
function_entry_t function_registry[256];
int registry_count = 0;

// Initialize the function registry
void init_function_registry(void) {
  register_function("editor-save", editorSave);
  register_function("move-cursor-beg-line", moveCursorBeginLine);
  register_function("move-cursor-end-line", moveCursorEndLine);
  register_function("editor-quit", editorQuit);
  register_function("move-cursor-up", editorMoveCursorUp);
  register_function("move-cursor-down", editorMoveCursorDown);
  register_function("move-cursor-right", editorMoveCursorRight);
  register_function("move-cursor-left", editorMoveCursorLeft);
  register_function("move-cursor-page-up", editorMoveCursorPageUp);
  register_function("move-cursor-page-down", editorMoveCursorPageDown);
  register_function("delete-previous-char", editorDelChar);
  register_function("delete-next-char", deleteNextChar);
  register_function("editor-insert-new-line", editorInsertNewLine);
}

// Register a function with a name
int register_function(const char *name, command_func_t func) {
  if (registry_count >= 256) {
    return -1; // Registry full
  }

  function_registry[registry_count].name = strdup(name);
  function_registry[registry_count].func = func;
  registry_count++;
  return 0;
}

// Find a function by name
command_func_t find_function(const char *name) {
  log_string("registry :");
  char *tmp = malloc(3 * sizeof(char));
  sprintf(tmp, "%d\n", registry_count);
  log_string(tmp);
  for (int i = 0; i < registry_count; i++) {
    if (strcmp(function_registry[i].name, name) == 0) {
      return function_registry[i].func;
    }
  }
  return NULL;
}

// Execute a command by name
int execute_command(const char *name, struct editorConfig *E) {
  log_string(name);
  command_func_t func = find_function(name);
  if (func) {
    func(E);
    return 0;
  }
  log_string("Unknown command: \n");
  return -1;
}

// Builtins tools

void moveCursorBeginLine(struct editorConfig *E) { E->cursor_x = 0; }

void moveCursorEndLine(struct editorConfig *E) {
  if (E->cursor_y < E->numrows) {
    E->cursor_x = E->row[E->cursor_y].size;
  }
}

void editorQuit(struct editorConfig *E) {
  log_string("time to quit\n");
  if (E->dirty && E->quit_times > 0) {
    editorSetStatusMessage(E,
                           "WARNING! Changes hasn't been saved. Press Ctrl-Q "
                           "another time to quit.");
    --E->quit_times;
    return;
  }
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
  disableRawMode(E);
  exit(0);
}


void editorMoveCursorUp(struct editorConfig *E) {
  editorMoveCursor(E, ARROW_UP);
}

void editorMoveCursorDown(struct editorConfig *E) {
  editorMoveCursor(E, ARROW_DOWN);
}

void editorMoveCursorRight(struct editorConfig *E) {
  editorMoveCursor(E, ARROW_RIGHT);
}

void editorMoveCursorLeft(struct editorConfig *E) {
  editorMoveCursor(E, ARROW_LEFT);
}

void deleteNextChar(struct editorConfig *E) {
  editorMoveCursorRight(E);
  editorDelChar(E);
}

void editorMoveCursorPageUp(struct editorConfig *E) {
  E->cursor_y = E->row_offset;
  int times = E->screenrows;
  while (--times) {
    editorMoveCursor(E, ARROW_UP);
  }
}

void editorMoveCursorPageDown(struct editorConfig *E) {
  E->cursor_y = E->row_offset + E->screenrows - 1;
  if (E->cursor_y > E->numrows) {
    E->cursor_y = E->numrows;
  }
  int times = E->screenrows;
  while (--times) {
    editorMoveCursor(E, ARROW_DOWN);
  }
}
