#ifndef BUILTINS_H_
#define BUILTINS_H_

#include "../blisp/include/config_tools.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include "../include/output.h"
#include "../include/input.h"
#include "data.h"
#include "file_io.h"

// Function pointer type for commands
typedef void (*command_func_t)(struct editorConfig *E);

// Structure to hold function mappings
typedef struct {
  const char *name;
  command_func_t func;
} function_entry_t;

// Function registry

void init_function_registry(void);
int register_function(const char *name, command_func_t func);
command_func_t find_function(const char *name);
int execute_command(const char *name, struct editorConfig *E);

void moveCursorBeginLine(struct editorConfig *E);
void moveCursorEndLine(struct editorConfig *E);
void editorQuit(struct editorConfig *E);

void editorMoveCursorUp(struct editorConfig *E);
void editorMoveCursorDown(struct editorConfig *E);
void editorMoveCursorLeft(struct editorConfig *E);
void editorMoveCursorRight(struct editorConfig *E);
void deleteNextChar(struct editorConfig *E);
void editorMoveCursorPageUp(struct editorConfig *E);
void editorMoveCursorPageDown(struct editorConfig *E);

#endif
