/**
 * @file builtins.c
 * @brief Built-in Lisp functions for editor operations
 * @details Provides Lisp bindings for core editor functionality including
 * cursor movement, file operations, keybinding management, and text
 * manipulation
 */

#include "../include/builtins.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include "../include/input.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/completion.h"
#include "../include/init.h"
#include "../include/utf8.h"
#include "../include/buffer_switcher.h"
#include "../include/list_popup.h"

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/


/**
 * @brief Finds a prefix configuration by name
 * @details Searches the prefix array for a prefix matching the given name.
 * Returns the first prefix (default) if no match is found.
 * @param prefix_name Name of the prefix to search for (max 64 chars)
 * @return Matching prefix_t structure, or E.prefix[0] if not found
 * @note Updates global editor state E
 */
struct prefix_t find_prefix(const char prefix_name[64]) {
  int i = E.number_of_prefix + 1;
  while (i--) {
    if (!strcmp(prefix_name, E.prefix[i].prefix_name)) {
      return E.prefix[i];
    }
  }
  return E.prefix[0];
}

/**
 * @brief Lisp function to bind a command to a key sequence
 * @details Registers a keybinding with an associated Lisp command and optional
 * prefix modifier. Dynamically extends the keybind array.
 * @param args Lisp list of 3 arguments: (key-sequence command prefix-name)
 *        - key_sequence (string): The key or key combination to bind
 *        - command (Lisp): The Lisp command/function to execute
 *        - prefix_name (string): Prefix modifier name
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E (key_binds array)
 */
Lisp mapKey(Lisp args, LispError *e, LispContext ctx) {
  /*
   * 3 arguments keybind command prefix
   */
  const char *key_sequence = lisp_string(lisp_car(args));
  void *memory_temp;
  args = lisp_cdr(args);
  // second argument
  const Lisp func = lisp_car(args);

  memory_temp =
      realloc(E.key_binds, ++E.number_of_keybinds * sizeof(struct keyBind_t));
  E.key_binds = (struct keyBind_t *)memory_temp;
  if (!E.key_binds)
    editorQuit(args, e, ctx);
  E.key_binds[E.number_of_keybinds - 1].key_sequence =
      (char *)malloc(1024 * sizeof(char));

  strncpy(E.key_binds[E.number_of_keybinds - 1].key_sequence, key_sequence, 50);

  E.key_binds[E.number_of_keybinds - 1].command = func;

  // Third argument
  args = lisp_cdr(args);
  const char *prefix_name = lisp_string(lisp_car(args));
  struct prefix_t prefix = find_prefix(prefix_name);

  E.key_binds[E.number_of_keybinds - 1].prefix_id = prefix.prefix_id;

  return lisp_null();
}

/**
 * @brief Lisp function to move cursor in a specified direction
 * @details Moves the editor cursor up, down, left, or right based on direction
 * string.
 * @param args Lisp list with one argument: direction string
 *        - "u": up, "d": down, "r": right, "l": left
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return Lisp boolean indicating whether movement was valid
 * @note Updates global editor state E
 * @see editorMoveCursor()
 */
Lisp moveCursor(Lisp args, LispError *e, LispContext ctx) {
  const char *direction = lisp_string(lisp_car(args));
  int is_in = 0;
  switch (direction[0]) {
  case 'u':
    is_in = editorMoveCursor(ARROW_UP);
    break;
  case 'd':
    is_in = editorMoveCursor(ARROW_DOWN);
    break;
  case 'r':
    is_in = editorMoveCursor(ARROW_RIGHT);
    break;
  case 'l':
    is_in = editorMoveCursor(ARROW_LEFT);
    break;
  default:
    break;
  }
  return lisp_make_bool(is_in);
}

/**
 * @brief bFrees all dynamically allocated editor structures
 * @details Releases memory for prefix table, keybinds, filename, and all rows.
 * Called during shutdown to prevent memory leaks.
 * @note Updates global editor state E
 */
void bFree_structs(void) {
  int i, j;
  for (i = 0; i < E.number_of_keybinds; ++i) {
    free(E.key_binds[i].key_sequence);
    E.key_binds[i].key_sequence = NULL;
  }
  free(E.key_binds);
  E.key_binds = NULL;
  E.number_of_keybinds = 0;

  free(E.hooks);
  E.hooks = NULL;
  E.number_of_hooks = 0;

  // Buffers are a circular doubly-linked list now, each node individually
  // allocated — capture `next` before freeing `b` (a do-while keyed off
  // b->buffer_id would dereference already-freed memory once the loop
  // wraps back around), and use the known count rather than an ID check
  // to know when to stop.
  if (E.buffers != NULL) {
    struct buffer_t *b = E.buffers;
    for (i = 0; i < E.number_of_buffer; ++i) {
      struct buffer_t *next = b->next;
      for (j = 0; j < b->numrows; ++j) {
        free(b->row[j].characters);
        free(b->row[j].attrs);
      }
      free(b->row);
      if (b->filename)
        free(b->filename);
      if (b->path)
        free(b->path);
      if (b->fullname)
        free(b->fullname);
      free(b);
      b = next;
    }
  }
  E.buffers = NULL;
  E.number_of_buffer = 0;
}

/**
 * @brief Lisp function to quit the editor
 * @details Closes editor with unsaved changes protection. Prompts user to
 * confirm quit after multiple attempts if file is dirty. Only requests the
 * exit here — actual teardown (deInitEditor/bFree_structs) happens in
 * main() after editorRunLoop() returns, never from inside this function.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates quit_times_buffer counter
 * @see main()
 */
Lisp editorQuit(Lisp args, LispError *e, LispContext ctx) {
  if (E.dirty && E.quit_times_buffer > 0) {
    editorSetStatusMessage("WARNING! Changes hasn't been saved. Press Ctrl-Q "
                           "another time to quit.");
    --E.quit_times_buffer;
    return lisp_null();
  }

  // deInitEditor() tears down the Lisp context itself (lisp_shutdown).
  // Calling it here, synchronously, would free the interpreter's heap
  // while this very function is still executing *inside* a live
  // lisp_apply/eval_r call chain — a use-after-free the moment that
  // machinery tries to unwind. Just request the exit; editorRunLoop()
  // checks exit_requested and returns cleanly, and main() does teardown
  // only once no Lisp evaluation is on the stack anymore.
  //
  // Terminal teardown deliberately isn't done here either: this runs
  // mid-iteration, and the rest of that iteration still calls
  // editorRefreshScreen(), which repainted the whole UI right back over
  // the cleared screen. editorRunLoop() restores the terminal once, after
  // the loop has actually stopped drawing.
  E.exit_requested = 1;
  return lisp_null();
}

/**
 * @brief Lisp function to save the current file
 * @details Wrapper around editorSave() for use in Lisp keybindings.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorSave()
 */

Lisp l_editorSplitScreenVertical(Lisp args, LispError *e, LispContext ctx) {
  if (E.number_of_buffer >= 2 && E.buffers != NULL) {
    splitScreenVertical(E.buffers->buffer_id, E.buffers->next->buffer_id);
  } else {
    editorSetStatusMessage("Need at least 2 buffers open");
  }
  return lisp_null();
}

Lisp l_editorSave(Lisp args, LispError *e, LispContext ctx) {
  appDebug("[EDITOR SAVE]");
  editorSave();
  return lisp_null();
}

/**
 * @brief Lisp function to insert a new line at cursor
 * @details Wrapper around editorInsertNewLine() for use in Lisp keybindings.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorInsertNewLine()
 */
Lisp l_editorInsertNewLine(Lisp args, LispError *e, LispContext ctx) {
  bufferInsertNewLine();

  return lisp_null();
}

/**
 * @brief Lisp function to insert a tab (spaces) at cursor
 * @details Inserts TAB_LENGTH spaces at the current cursor position.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Uses E.constantes.TAB_LENGTH for indentation width
 */
Lisp l_editorInserTab(Lisp args, LispError *e, LispContext ctx) {
  Utf8Char space;
  *space.character = ' ';
  space.length = 1;
  for (int i = 0; i < E.constantes.TAB_LENGTH; ++i) {
    bufferInsertChar(&space);
  }

  return lisp_null();
}

/**
 * @brief Lisp function to move cursor to beginning of line
 * @details Moves cursor to column 0 of the current line.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E
 */
Lisp moveCursorBeginLine(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  buf->x = 0;
  return lisp_null();
}

/**
 * @brief Lisp function to move cursor to end of line
 * @details Moves cursor to the end of the current line content.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E
 */
Lisp moveCursorEndLine(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  buf->x = buf->row[buf->y].size;
  return lisp_null();
}

/**
 * @brief Lisp function to delete character before cursor
 * @details Wrapper around editorDelChar() for use in Lisp keybindings.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorDelChar()
 */
Lisp deletePreviousChar(Lisp args, LispError *e, LispContext ctx) {
  bufferDelChar();
  return lisp_null();
}

/**
 * @brief Lisp function to undo the most recent edit (or coalesced run of
 * edits — see undoRecord() in buffer.c) on the current buffer.
 * @return lisp_null()
 * @see bufferUndo()
 */
Lisp editorUndo(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (buf)
    bufferUndo(buf);
  return lisp_null();
}

/**
 * @brief Lisp function to redo the most recently undone edit on the
 * current buffer.
 * @return lisp_null()
 * @see bufferRedo()
 */
Lisp editorRedo(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (buf)
    bufferRedo(buf);
  return lisp_null();
}

/**
 * @brief Lisp function to move cursor up by one screen
 * @details Scrolls up one full screen height, moving cursor to top of visible
 * area.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E
 * @see editorMoveCursor()
 */
Lisp editorMoveCursorPageUp(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  active->cursor_y = active->y_offset;
  int times = E.screenrows;
  while (--times) {
    editorMoveCursor(ARROW_UP);
  }
  return lisp_null();
}

/**
 * @brief Lisp function to move cursor down by one screen
 * @details Scrolls down one full screen height, moving cursor to bottom of
 * visible area.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E
 * @see editorMoveCursor()
 */
Lisp editorMoveCursorPageDown(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  active->cursor_y = active->y_offset + E.screenrows - 1;
  if (active->cursor_y > buffer->numrows) {
    active->cursor_y = buffer->numrows;
  }
  int times = E.screenrows;
  while (--times) {
    editorMoveCursor(ARROW_DOWN);
  }

  return lisp_null();
}

/**
 * @brief Lisp function to open a file
 * @details Prompts user for filename and opens the file for editing.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E
 * @see editorOpen()
 * @see editorPrompt()
 */
Lisp editorOpenFile(Lisp args, LispError *e, LispContext ctx) {
  appDebug("[EDITOR OPEN FILE]\n");
  Utf8Char * pwd = stringToUtf8((unsigned char *)getenv("PWD"));
  Utf8Char *filename = editorPrompt("Open : %u", pwd, 1);
  if (filename) {
    EditorPane *active = splitScreenGetActivePane();
    active->buffer_id = bufferCreate(filename, READ_AND_WRITE, FILE_BUFF);
  }
  free(pwd);
  free(filename);

  return lisp_null();
}

/**
 * @brief Lisp function to jump straight to the editor's own config file
 * @details (editor-open-config) opens ~/.beluga/config/init.lisp (the
 * exact path E.init_file_path was already resolved to at startup) in
 * the active pane — no prompt, unlike editor-open-file.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp editorOpenConfig(Lisp args, LispError *e, LispContext ctx) {
  Utf8Char *path = stringToUtf8((unsigned char *) E.init_file_path);
  EditorPane *active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
  free(path);
  return lisp_null();
}

// ─── Theme ──────────────────────────────────────────────────────────────────

/**
 * @brief Lisp function to set one theme color
 * @details (editor-set-theme-color role r g b) — role is one of
 * "background", "keyword", "type", "string", "comment", "number",
 * "default"; r/g/b are 0-255. Writes straight into E.theme, replacing
 * whatever the fallback (or an earlier-loaded theme package) set —
 * lets a theme be an ordinary package (packages/theme-dark,
 * packages/theme-light, or your own) instead of a hardcoded C palette.
 * @param args Lisp list of 4 arguments: (role r g b)
 * @return lisp_null()
 */
Lisp editorSetThemeColor(Lisp args, LispError *e, LispContext ctx) {
  const char *role = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  int r = (int) lisp_int(lisp_car(args));
  args = lisp_cdr(args);
  int g = (int) lisp_int(lisp_car(args));
  args = lisp_cdr(args);
  int b = (int) lisp_int(lisp_car(args));

  int is_background = !strcmp(role, "background");
  char *ansi = malloc(24); // "\x1b[38;2;255;255;255m" + NUL fits well within this
  snprintf(ansi, 24, "\x1b[%d;2;%d;%d;%dm", is_background ? 48 : 38, r, g, b);

  if (is_background)                         E.theme.BACKGROUND_COLOR = ansi;
  else if (!strcmp(role, "keyword"))         E.theme.COLOR_KEYWORD = ansi;
  else if (!strcmp(role, "type"))            E.theme.COLOR_TYPE = ansi;
  else if (!strcmp(role, "string"))          E.theme.COLOR_STRING = ansi;
  else if (!strcmp(role, "comment"))         E.theme.COLOR_COMMENT = ansi;
  else if (!strcmp(role, "number"))          E.theme.COLOR_NUMBER = ansi;
  else if (!strcmp(role, "default"))         E.theme.COLOR_DEFAULT = ansi;
  else
  {
    free(ansi);
    editorSetStatusMessage("Unknown theme color role: %s", role);
  }

  return lisp_null();
}

/**
 * @brief Lisp function to open a file at a given path, no prompt
 * @details (editor-open-file-path path) — same as editor-open-file but
 * takes the path as a string argument instead of prompting for one.
 * Opens in the active pane. Lets Lisp code (e.g. the file-explorer
 * package) open a file it already knows the path to.
 * @param args Lisp list of 1 argument: (path)
 * @return lisp_null()
 */
Lisp editorOpenFilePath(Lisp args, LispError *e, LispContext ctx) {
  const char *path_str = lisp_string(lisp_car(args));
  Utf8Char *path = stringToUtf8((unsigned char *) path_str);
  EditorPane *active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
  free(path);
  return lisp_null();
}

// ─── Directory listing / scratch buffers (file explorer, tree dump, ...) ────

/**
 * @brief Lisp function returning the process's actual current working
 * directory (via getcwd(), not the possibly-stale $PWD env var)
 * @details (editor-pwd) — used as the default project root for the file
 * explorer and tree dump, same role editor-open-file's own prompt
 * placeholder already plays for picking a file interactively.
 * @param args Lisp arguments (unused)
 * @return Lisp string, or "" if getcwd() fails
 */
Lisp editorPwd(Lisp args, LispError *e, LispContext ctx) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL)
    return lisp_make_string2("", ctx);
  return lisp_make_string2(cwd, ctx);
}

/**
 * @brief Lisp function to list a directory's entries
 * @details (list-directory path) — returns a list of (name . is-dir)
 * pairs, one per entry, excluding "." and "..". Order is whatever
 * readdir() gives (filesystem-dependent, not sorted) — sort in Lisp if
 * you need a stable order. Returns '() for a path that doesn't exist or
 * isn't a directory.
 * @param args Lisp list of 1 argument: (path)
 * @return Lisp list of (name . is-dir) pairs
 */
Lisp listDirectory(Lisp args, LispError *e, LispContext ctx) {
  const char *path = lisp_string(lisp_car(args));
  DIR *dir = opendir(path);
  if (!dir) return lisp_null();

  Lisp result = lisp_null();
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

    struct stat st;
    int is_dir = (stat(full_path, &st) == 0) && S_ISDIR(st.st_mode);

    Lisp pair = lisp_cons(lisp_make_string2(entry->d_name, ctx),
                           lisp_make_bool(is_dir), ctx);
    result = lisp_cons(pair, result, ctx);
  }
  closedir(dir);
  return result;
}

/**
 * @brief Lisp function to switch the active pane to a specific buffer
 * @details (editor-switch-to-buffer buffer-id) — a direct, non-interactive
 * counterpart to editor-switch-buffer-popup, for Lisp code that already
 * knows which buffer it wants (e.g. after editor-scratch-open).
 * @param args Lisp list of 1 argument: (buffer-id)
 * @return lisp_null()
 */
Lisp editorSwitchToBuffer(Lisp args, LispError *e, LispContext ctx) {
  int buffer_id = (int) lisp_int(lisp_car(args));
  bufferSwitch(buffer_id);
  return lisp_null();
}

/**
 * @brief Lisp function to create (or find, by name) a read-only scratch
 * buffer for programmatically-generated content
 * @details (editor-scratch-open name) — doesn't touch panes or the
 * active buffer at all; just returns the buffer's id so the caller can
 * fill it via editor-scratch-set-lines and/or place it in a pane
 * however it wants (see editor-explorer-sidebar-open). Calling this
 * again with the same name reuses the existing buffer (same dedup
 * bufferCreate() already does by filename).
 * @param args Lisp list of 1 argument: (name)
 * @return Lisp int: the buffer's id
 */
Lisp editorScratchOpen(Lisp args, LispError *e, LispContext ctx) {
  const char *name = lisp_string(lisp_car(args));
  Utf8Char *uname = stringToUtf8((unsigned char *) name);
  int id = bufferCreate(uname, READ_ONLY, EXPLORER_BUFF);
  free(uname);
  return lisp_make_int(id);
}

/**
 * @brief Lisp function to replace a scratch buffer's entire content
 * @details (editor-scratch-set-lines buffer-id lines) — lines is a list
 * of strings, one per row. Only affects EXPLORER_BUFF buffers (silently
 * does nothing otherwise, so a stale/wrong id can't corrupt a real file
 * buffer's content).
 * @param args Lisp list of 2 arguments: (buffer-id lines)
 * @return lisp_null()
 */
Lisp editorScratchSetLines(Lisp args, LispError *e, LispContext ctx) {
  int buffer_id = (int) lisp_int(lisp_car(args));
  args = lisp_cdr(args);
  Lisp lines = lisp_car(args);

  struct buffer_t *buf = bufferFindById(buffer_id);
  if (!buf || buf->type != EXPLORER_BUFF)
    return lisp_null();

  for (int i = 0; i < buf->numrows; i++) {
    free(buf->row[i].characters);
    free(buf->row[i].attrs);
  }
  free(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
  buf->x = 0;
  buf->y = 0;

  while (lisp_is_pair(lines)) {
    const char *line_str = lisp_string(lisp_car(lines));
    Utf8Char *uline = stringToUtf8((unsigned char *) line_str);
    int len = utf8StringLen(uline) - 1; // utf8StringLen() counts the terminator too
    bufferInsertRow(buf, buf->numrows, uline, len);
    free(uline);
    lines = lisp_cdr(lines);
  }
  buf->dirty = 0;

  return lisp_null();
}

/**
 * @brief Lisp function to open the file-explorer sidebar
 * @details (editor-explorer-sidebar-open name width) — creates/reuses a
 * scratch buffer called `name`, splits the screen with it as a
 * fixed-width left pane and whatever was in the active pane as the
 * right pane, and focuses the sidebar (switching to the "explorer"
 * prefix — see splitScreenWithSidebar()). The explorer package fills
 * the sidebar's content afterward via editor-scratch-set-lines.
 * @param args Lisp list of 2 arguments: (name width)
 * @return Lisp int: the sidebar buffer's id
 */
Lisp editorExplorerSidebarOpen(Lisp args, LispError *e, LispContext ctx) {
  const char *name = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  int width = (int) lisp_int(lisp_car(args));

  EditorPane *active = splitScreenGetActivePane();
  int main_id = active->buffer_id;

  Utf8Char *uname = stringToUtf8((unsigned char *) name);
  int sidebar_id = bufferCreate(uname, READ_ONLY, EXPLORER_BUFF);
  free(uname);

  splitScreenWithSidebar(sidebar_id, main_id, width);

  return lisp_make_int(sidebar_id);
}

/**
 * @brief Lisp function to close the file-explorer sidebar
 * @details (editor-explorer-close) — collapses back to fullscreen,
 * keeping whichever pane holds real content (see
 * splitScreenCloseSidebar()).
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp editorExplorerClose(Lisp args, LispError *e, LispContext ctx) {
  splitScreenCloseSidebar();
  return lisp_null();
}

Lisp editorOpenTerminal(Lisp args, LispError *e, LispContext ctx) {
  appDebug("[EDITOR OPEN TERMINAL]\n");
  EditorPane *active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(NULL, READ_AND_WRITE, TERMINAL_BUFF);
  return lisp_null();
}

/**
 * @brief Lisp function: M-x style "execute command"
 * @details Prompts for a command name, looks it up as a symbol in the
 * global Lisp environment, and calls it with no arguments if it's bound
 * to a callable value — a builtin (registerBuiltin) or a user-defined
 * function (define/lambda), the same environment map-key resolves
 * against. Bound to ALT-x in the default config.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 */
Lisp editorExecuteCommand(Lisp args, LispError *e, LispContext ctx) {
  Utf8Char *name = editorPrompt("M-x: %u", NULL, 0);
  if (!name)
    return lisp_null();

  char *name_str = (char *)utf8ToString(name);
  free(name);

  int present = 0;
  Lisp sym = lisp_make_symbol(name_str, ctx);
  // lisp_env(ctx) fetches the *current* global env fresh — the interpreter's
  // GC is a moving/compacting collector that relocates the whole heap and
  // updates ctx.p->env internally on every collection, but never touches any
  // copy of that Lisp value a caller cached earlier. A cached copy (E.env
  // used to be exactly that) goes stale the moment a single collection runs
  // — which happens once, unconditionally, at the end of lisp_lib_load(),
  // before this function is ever even reachable.
  Lisp val = lisp_env_lookup(lisp_env(ctx), sym, &present);

  if (!present) {
    editorSetStatusMessage("No such command: %s", name_str);
    free(name_str);
    return lisp_null();
  }

  if (lisp_type(val) != LISP_FUNC && lisp_type(val) != LISP_LAMBDA) {
    editorSetStatusMessage("Not a command: %s", name_str);
    free(name_str);
    return lisp_null();
  }

  LispError call_error = LISP_ERROR_NONE;
  lisp_apply(val, lisp_null(), &call_error, ctx);
  if (call_error != LISP_ERROR_NONE)
    editorSetStatusMessage("Error running: %s", name_str);

  free(name_str);
  return lisp_null();
}

/**
 * @brief Lisp function to insert a character
 * @details Extracts a character from Lisp string argument and inserts it at
 * cursor.
 * @param args Lisp list with one string argument
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Uses first character of the string argument
 */
Lisp editorPrintC(Lisp args, LispError *e, LispContext ctx) {
  const char *src = lisp_string(lisp_car(args));
  if (src && src[0]) {
    Utf8Char ch;
    utf8CreateChar((const unsigned char *) src, &ch);
    bufferInsertChar(&ch);
  }
  return lisp_null();
}

/**
 * @brief Lisp function to load and execute a package
 * @details Loads a Lisp package from the user's packages directory
 * (~/.beluga/packages/<package_name>/init.lisp) and evaluates it.
 * @param args Lisp list with one argument: package name (string)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Package files must be valid Lisp code
 * @note Package directory defaults to ~/.beluga/packages/
 */
Lisp addPackage(Lisp args, LispError *e, LispContext ctx) {
  const char *package_name = lisp_string(lisp_car(args));
  appDebug("%s\n", package_name);
  const char *home = getenv("HOME");
  if (!home) {
    return lisp_null();
  }

  char *package_dir;
  if (asprintf(&package_dir, "%s/.beluga/packages/%s/init.lisp", home,
               package_name) == -1) {
    return lisp_null();
  }

  FILE *fd_package = fopen(package_dir, "r");
  if (!fd_package) {
    free(package_dir);
    return lisp_null();
  }

  lisp_eval(lisp_read_file(fd_package, &E.ctx_error, E.ctx), &E.ctx_error,
            E.ctx);
  fclose(fd_package);
  free(package_dir);

  return lisp_null();
}

Lisp editorSwitchNextBuffer(Lisp args, LispError *e, LispContext ctx) {
  appDebug("switch buffer\n");
  // buffer_id is a monotonically-increasing counter, not a dense 0..N-1
  // array index (especially once any buffer's been closed, leaving gaps),
  // so "(id + 1) % count" can land on an id that doesn't exist. Walk the
  // actual list instead.
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *current = bufferFindById(active->buffer_id);
  if (current) {
    active->buffer_id = current->next->buffer_id;
  }

  return lisp_null();
}

/**
 * @brief Lisp function to open the CTRL-x b buffer switcher popup
 * @details Pops a list of every open buffer up in the middle of the
 * screen; ARROW-UP/DOWN (or CTRL-p/CTRL-n) move the highlight, ENTER
 * switches to it, ESCAPE cancels. Key handling and rendering happen in
 * buffer_switcher.c, hooked into editorProcessKeypress()/
 * editorRefreshScreen() while the popup is visible.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 */
Lisp editorSwitchBufferPopup(Lisp args, LispError *e, LispContext ctx) {
  bufferSwitcherOpen();
  return lisp_null();
}

Lisp editorSwitchNextPane(Lisp args, LispError *e, LispContext ctx) {
  splitScreenSwitchPane();
  return lisp_null();
}

Lisp editorUnifiedPanes(Lisp args, LispError *e, LispContext ctx) {
  if (E.layout.num_panes - 1) {
    splitScreenUnify();
  }
  return lisp_null();
}

/**
 * @brief Lisp function to search for text
 * @details Wrapper around editorFind() for use in Lisp keybindings.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorFind()
 */
Lisp bufferFind_L(Lisp args, LispError *e, LispContext ctx) {
  appDebug("LispFind\n");
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  bufferFind(buffer);
  return lisp_null();
}

/**
 * @brief Lisp function to search for text
 * @details Wrapper around editorFind() for use in Lisp keybindings.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorFind()
 */
Lisp bufferFindReverse_L(Lisp args, LispError *e, LispContext ctx) {
  appDebug("LispFind\n");
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  bufferFindReverse(buffer);
  return lisp_null();
}

/**
 * @brief Lisp function to read character at cursor
 * @details Returns the character at the current cursor position as a Lisp
 * character. Returns 'a' if at end of line.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return Lisp character object representing the character at cursor
 */
Lisp editorReadChar_L(Lisp args, LispError *e, LispContext ctx) {
  Lisp returned_char;
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
    returned_char = lisp_make_char('a');
#if 0
  if (buffer->row[buffer->y].chars[buffer->x] == 0) {
    returned_char = lisp_make_char('a');
  } else {
    returned_char = lisp_make_char(buffer->row[buffer->y].chars[buffer->x]);
  }
#endif
  return returned_char;
}

/**
 * @brief Lisp function to set the current prefix mode
 * @details Changes the editor's prefix state to a named prefix, affecting which
 * keybindings are active. Updates status message to show active prefix.
 * @param args Lisp list with one argument: prefix name (string)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E (prefix_state)
 * @see find_prefix()
 */
Lisp editorSetPrefix(Lisp args, LispError *e, LispContext ctx) {
  /*
   * Set the prefix state of editor to the prefix in argument
   */
  const char *prefix_name = lisp_string(lisp_car(args));
  struct prefix_t prefix = find_prefix(prefix_name);
  E.prefix_state = prefix.prefix_id;
  editorSetStatusMessage("prefix %s", prefix.prefix_name);
  appDebug("%s set\n", prefix_name);

  return lisp_null();
}

/**
 * @brief Lisp function to define a new prefix modifier
 * @details Registers a named prefix modifier that can be used with keybindings
 * to create context-aware command sequences (e.g., Ctrl-X as a prefix).
 * (add-prefix "name" [sticky?]) — sticky? defaults to #f (matching every
 * existing prefix's behavior: one keystroke under it runs, then
 * executeKeyBind() auto-resets to "no-prefix" unless the command itself
 * changed the prefix). Pass #t for a prefix meant to stay active across
 * multiple keystrokes instead, e.g. arrow-key navigation in a sidebar.
 * @param args Lisp list of 1 or 2 arguments: (name [sticky?])
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E (prefix array)
 */
Lisp editorPrefix(Lisp args, LispError *e, LispContext ctx) {
  E.prefix = (struct prefix_t *)realloc(E.prefix, (++(E.number_of_prefix) + 1) *
                                                      sizeof(struct prefix_t));
  E.prefix[E.number_of_prefix].prefix_id = E.number_of_prefix;
  strncpy(E.prefix[E.number_of_prefix].prefix_name, lisp_string(lisp_car(args)),
          64);
  args = lisp_cdr(args);
  E.prefix[E.number_of_prefix].sticky = lisp_is_pair(args) && lisp_bool(lisp_car(args));
  return lisp_null();
}

/**
 * @brief Lisp function to paste the system clipboard at the cursor
 * @details Reads the clipboard via editorGetClipboard() (pbpaste/xclip)
 * and inserts it character-by-character, decoding each UTF-8 character
 * with utf8CreateChar(). A literal '\n' starts a new line via
 * bufferInsertNewLine() rather than being inserted as a raw character,
 * so multi-line clipboard content actually produces multiple lines.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorGetClipboard()
 */
Lisp editorPaste(Lisp args, LispError *e, LispContext ctx) {
  char *text = editorGetClipboard();
  if (!text)
    return lisp_null();

  const unsigned char *p = (const unsigned char *) text;
  while (*p) {
    if (*p == '\n') {
      bufferInsertNewLine();
      p++;
    } else {
      Utf8Char ch;
      p += utf8CreateChar(p, &ch);
      bufferInsertChar(&ch);
    }
  }

  free(text);
  return lisp_null();
}

/**
 * @brief Lisp function to cut from the cursor to end of line
 * @details Copies the cut text to the system clipboard first (via
 * editorSetClipboard()), then truncates the row at the cursor. A no-op
 * if the cursor is already at the end of the line.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorCopyEndLine()
 */
Lisp editorCutEndLine(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  if (!buffer || buffer->y >= buffer->numrows)
    return lisp_null();

  Row *row = &buffer->row[buffer->y];
  int count = row->size - buffer->x;
  if (count <= 0)
    return lisp_null();

  char *text = (char *) utf8ToNString(&row->characters[buffer->x], count);
  editorSetClipboard(text, (int) strlen(text));
  free(text);

  // Goes through bufferDeleteRegion() (the same path region-kill and LSP
  // edits use) rather than truncating row->size directly, so this is
  // undoable — it used to bypass undo recording entirely.
  bufferDeleteRegion(buffer, buffer->y, buffer->x, buffer->y, row->size);
  buffer->b_has_changed = 1;

  return lisp_null();
}

/**
 * @brief Lisp function to copy from the cursor to end of line
 * @details Like editorCutEndLine() but non-destructive — copies to the
 * system clipboard without deleting anything.
 * @param args Lisp arguments (unused)
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @see editorCutEndLine()
 */
Lisp editorCopyEndLine(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  if (!buffer || buffer->y >= buffer->numrows)
    return lisp_null();

  Row *row = &buffer->row[buffer->y];
  int count = row->size - buffer->x;
  if (count <= 0)
    return lisp_null();

  char *text = (char *) utf8ToNString(&row->characters[buffer->x], count);
  editorSetClipboard(text, (int) strlen(text));
  free(text);

  return lisp_null();
}

/**
 * @brief Lisp function to set (or clear) the mark at the cursor.
 * @details Emacs-style: the region is whatever's between the mark and the
 * cursor ("point"), recomputed live as the cursor moves — see
 * rowSelectionRange() in output.c for the rendering side and
 * bufferRegionBounds() for how the two get normalized into document order.
 * Calling this again while already active clears the mark instead of
 * moving it, so it also serves as "cancel selection".
 * @return lisp_null()
 */
Lisp editorSetMark(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  if (buf->mark_active) {
    buf->mark_active = 0;
    editorSetStatusMessage("Mark cleared");
  } else {
    buf->mark_active = 1;
    buf->mark_x = buf->x;
    buf->mark_y = buf->y;
    editorSetStatusMessage("Mark set");
  }
  return lisp_null();
}

/**
 * @brief Lisp function to cut the active region to the clipboard.
 * @details No-op with a status message if no mark is active. Clears the
 * mark afterward, same as real Emacs' C-w.
 * @return lisp_null()
 * @see editorSetMark(), editorCopyRegion()
 */
Lisp editorKillRegion(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf || !buf->mark_active) {
    editorSetStatusMessage("No region (CTRL-x m to set the mark first)");
    return lisp_null();
  }

  int sr, sc, er, ec;
  bufferRegionBounds(buf, &sr, &sc, &er, &ec);
  char *text = bufferGetRegionText(buf, sr, sc, er, ec);
  if (text) {
    editorSetClipboard(text, (int) strlen(text));
    free(text);
  }
  bufferDeleteRegion(buf, sr, sc, er, ec);
  buf->mark_active = 0;
  buf->b_has_changed = 1;
  return lisp_null();
}

/**
 * @brief Lisp function to copy the active region to the clipboard without
 * deleting it. No-op with a status message if no mark is active. Clears
 * the mark afterward, same as real Emacs' M-w.
 * @return lisp_null()
 * @see editorSetMark(), editorKillRegion()
 */
Lisp editorCopyRegion(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf || !buf->mark_active) {
    editorSetStatusMessage("No region (CTRL-x m to set the mark first)");
    return lisp_null();
  }

  int sr, sc, er, ec;
  bufferRegionBounds(buf, &sr, &sc, &er, &ec);
  char *text = bufferGetRegionText(buf, sr, sc, er, ec);
  if (text) {
    editorSetClipboard(text, (int) strlen(text));
    free(text);
  }
  buf->mark_active = 0;
  return lisp_null();
}

Lisp editorMoveBegBuffer(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  buffer->x = 0;
  buffer->y = 0;
  return lisp_null();
}

Lisp editorMoveEndBuffer(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  buffer->x = buffer->row[buffer->numrows - 1].size;
  buffer->y = buffer->numrows - 1;
  return lisp_null();
}

Lisp editorAutoComplete(Lisp args, LispError *e, LispContext ctx) {
    if (!E.constantes.LSP) {
        return lisp_null();
      }
      appDebug("editor-auto-complete\n");
      EditorPane *active = splitScreenGetActivePane();
      struct buffer_t *buffer = bufferFindById(active->buffer_id);
      lspRequestCompletion(
          E.lsp_client, buffer,
          buffer->y,
          buffer->x,
          active->cursor_x + active->origin_x + GUTTER_WIDTH,
          active->cursor_y + active->origin_y
      );
      return lisp_null();
}

// ─── Hooks ──────────────────────────────────────────────────────────────────

/**
 * @brief Lisp function to register a callback for a named editor event
 * @details (add-hook "event-name" fn) — fn must be a callable value (a
 * lambda or a symbol bound to one), not the '(quoted-list) convention
 * map-key uses. Multiple hooks may share an event name; they all run, in
 * registration order, when runHooks() fires that event.
 * @param args Lisp list of 2 arguments: (event-name fn)
 * @return lisp_null()
 * @note Updates global editor state E (hooks array)
 */
Lisp addHook(Lisp args, LispError *e, LispContext ctx) {
  const char *event_name = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  Lisp callback = lisp_car(args);

  void *mem = realloc(E.hooks, (E.number_of_hooks + 1) * sizeof(struct hookBind_t));
  if (!mem)
    return lisp_null();
  E.hooks = (struct hookBind_t *)mem;

  strncpy(E.hooks[E.number_of_hooks].event_name, event_name, 63);
  E.hooks[E.number_of_hooks].event_name[63] = '\0';
  E.hooks[E.number_of_hooks].callback = callback;
  E.number_of_hooks++;

  return lisp_null();
}

/**
 * @brief Runs every hook registered for event_name, passing it args
 * @details Called from C at specific editor lifecycle points (see
 * builtins.h for the current list of events and their argument lists).
 * Errors inside a hook are logged via appDebug and otherwise swallowed —
 * one misbehaving hook must not be able to take the editor down, matching
 * the fire-and-forget error handling already used for keybindings.
 * @param event_name Event identifier, matched verbatim against add-hook calls
 * @param args Already-evaluated Lisp list of arguments passed to each callback
 */
void runHooks(const char *event_name, Lisp args) {
  for (int i = 0; i < E.number_of_hooks; i++) {
    if (strcmp(E.hooks[i].event_name, event_name) != 0)
      continue;
    LispError hook_error = LISP_ERROR_NONE;
    lisp_apply(E.hooks[i].callback, args, &hook_error, E.ctx);
    if (hook_error != LISP_ERROR_NONE)
      appDebug("[HOOK] \"%s\" callback raised an error\n", event_name);
  }
}

// ─── Buffer/cursor introspection ───────────────────────────────────────────

/**
 * @brief Lisp function to read the text of a buffer line
 * @details (line-text) returns the current line; (line-text n) returns
 * line n (0-based). Out-of-range rows return an empty string rather than
 * erroring, so callers can write e.g. (line-text (- (cursor-row) 1))
 * near the top of a buffer without a separate bounds check.
 * @param args Lisp list of 0 or 1 arguments: ([row])
 * @return Lisp string
 */
Lisp lineTextAt(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_make_string2("", ctx);

  int row = lisp_is_pair(args) ? (int)lisp_int(lisp_car(args)) : buf->y;
  if (row < 0 || row >= buf->numrows)
    return lisp_make_string2("", ctx);

  char *text = (char *)utf8ToString(buf->row[row].characters);
  Lisp result = lisp_make_string2(text, ctx);
  free(text);
  return result;
}

/**
 * @brief Lisp function returning the cursor's 0-based row in the active buffer
 */
Lisp cursorRowLisp(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  return lisp_make_int(buf ? buf->y : 0);
}

/**
 * @brief Lisp function returning the cursor's 0-based column in the active buffer
 */
Lisp cursorColLisp(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  return lisp_make_int(buf ? buf->x : 0);
}

/**
 * @brief Lisp function returning the active buffer's row count
 */
Lisp bufferLineCountLisp(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  return lisp_make_int(buf ? buf->numrows : 0);
}

/**
 * @brief Lisp function returning the active buffer's absolute file path
 * @details Empty string for a buffer with no backing file (e.g. a
 * terminal buffer never gets a real path here).
 */
Lisp bufferFilenameLisp(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf || !buf->fullname)
    return lisp_make_string2("", ctx);

  char *text = (char *)utf8ToString(buf->fullname);
  Lisp result = lisp_make_string2(text, ctx);
  free(text);
  return result;
}

/**
 * @brief Lisp function to move the cursor to an arbitrary row/col
 * @details (editor-goto-line row [col]) — row is 0-based and clamped to
 * the buffer's line range; col defaults to 0 and is clamped to that row's
 * length. Doesn't touch scroll offsets directly — editorScroll() brings
 * the new position into view on the next redraw, same as any other
 * cursor-moving builtin.
 * @param args Lisp list of 1 or 2 arguments: (row [col])
 * @return lisp_null()
 */
Lisp editorGotoLine(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  if (!buffer || buffer->numrows == 0)
    return lisp_null();

  int row = (int) lisp_int(lisp_car(args));
  if (row < 0) row = 0;
  if (row >= buffer->numrows) row = buffer->numrows - 1;

  args = lisp_cdr(args);
  int col = lisp_is_pair(args) ? (int) lisp_int(lisp_car(args)) : 0;
  if (col < 0) col = 0;
  if (col > buffer->row[row].size) col = buffer->row[row].size;

  buffer->y = row;
  buffer->x = col;
  return lisp_null();
}

/**
 * @brief Lisp function to open a generic modal list picker
 * @details (editor-list-popup title items on-select [on-cancel]) — a
 * primitive any Lisp package can use to show a selectable list without
 * writing its own C popup. `items` is a list of (label . value) pairs
 * where label is a string; `on-select` is called with the chosen pair's
 * cdr when the user presses ENTER, `on-cancel` (optional) with no
 * arguments if they press ESC instead. See list_popup.h.
 * @param args Lisp list of 3 or 4 arguments: (title items on-select [on-cancel])
 * @return lisp_null()
 */
Lisp editorListPopup(Lisp args, LispError *e, LispContext ctx) {
  const char *title = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  Lisp items = lisp_car(args);
  args = lisp_cdr(args);
  Lisp on_select = lisp_car(args);
  args = lisp_cdr(args);
  Lisp on_cancel = lisp_is_pair(args) ? lisp_car(args) : lisp_null();

  listPopupOpen(title, items, on_select, on_cancel);
  return lisp_null();
}

/**
 * @brief Lisp function to register a language server for a file extension
 * @details (lsp-server "ext" "language-id" "command" "arg1" "arg2" ...)
 * e.g. (lsp-server "c" "c" "clangd") or
 * (lsp-server "py" "python" "pyright-langserver" "--stdio"). The server is
 * only ever actually spawned for the first matching file opened in a
 * session (see bufferMaybeStartLsp) — registering it here just tells
 * beluga what to run and what LSP languageId to announce.
 * @param args Lisp list: (extension language-id command args...)
 * @return lisp_null()
 * @note Updates global editor state E (lsp_servers array)
 */
Lisp lspRegisterServer(Lisp args, LispError *e, LispContext ctx) {
  const char *ext = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  const char *lang_id = lisp_string(lisp_car(args));
  args = lisp_cdr(args);

  void *mem = realloc(E.lsp_servers,
                       (E.number_of_lsp_servers + 1) * sizeof(struct lspServerConfig_t));
  if (!mem)
    return lisp_null();
  E.lsp_servers = (struct lspServerConfig_t *)mem;

  struct lspServerConfig_t *cfg = &E.lsp_servers[E.number_of_lsp_servers];
  memset(cfg, 0, sizeof(*cfg));
  strncpy(cfg->extension, ext, sizeof(cfg->extension) - 1);
  strncpy(cfg->language_id, lang_id, sizeof(cfg->language_id) - 1);

  int argc = 0;
  while (!lisp_is_null(args) && argc < LSP_SERVER_MAX_ARGS) {
    cfg->argv[argc] = strdup(lisp_string(lisp_car(args)));
    argc++;
    args = lisp_cdr(args);
  }
  cfg->argv[argc] = NULL;

  E.number_of_lsp_servers++;
  return lisp_null();
}

/**
 * @brief Lisp function to jump to the definition of the symbol at the
 * cursor. Async: the actual jump happens once the server responds — see
 * lsp_on_definition_response() in completion.c.
 * @return lisp_null()
 */
Lisp lspDefinition(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY)
    return lisp_null();

  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  lspRequestDefinition(E.lsp_client, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function to show hover info (type/doc) for the symbol at the
 * cursor in the status bar. Async: see lsp_on_hover_response().
 * @return lisp_null()
 */
Lisp lspHover(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY)
    return lisp_null();

  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  lspRequestHover(E.lsp_client, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function to rename the symbol at the cursor across every
 * file the server reports. Prompts for the new name, then blocks briefly
 * (up to 3s — see lspAwaitResponse()) waiting for the server's
 * WorkspaceEdit, which is applied to every affected buffer (opening files
 * that aren't already open). Nothing is auto-saved — each touched buffer
 * is just marked dirty, same as any other edit.
 * @return lisp_null()
 */
Lisp lspRename(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }

  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  Utf8Char *name_input = editorPrompt("New name: %s (ESC to cancel)", NULL, 0);
  if (!name_input)
    return lisp_null();

  char *new_name = (char *) utf8ToString(name_input);
  free(name_input);
  if (new_name[0] == '\0') {
    free(new_name);
    return lisp_null();
  }

  lspRequestRename(E.lsp_client, buf, buf->y, buf->x, new_name);
  free(new_name);
  return lisp_null();
}

/**
 * @brief Lisp function to format the current buffer via the LSP server.
 * Blocks briefly (up to 3s) for the response — meant to be wired to the
 * "before-save" hook so the formatted result is what actually gets
 * written to disk, not the pre-format content. A no-op (silently) if LSP
 * isn't ready or the server doesn't support formatting.
 * @return lisp_null()
 */
Lisp lspFormatBuffer(Lisp args, LispError *e, LispContext ctx) {
  (void) args; (void) e; (void) ctx;
  if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY)
    return lisp_null();

  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  if (!buf || buf->type != FILE_BUFF)
    return lisp_null();

  lspRequestFormatting(E.lsp_client, buf);
  return lisp_null();
}
