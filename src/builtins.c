/**
 * @file builtins.c
 * @brief Built-in Lisp functions for editor operations
 * @details Provides Lisp bindings for core editor functionality including
 * cursor movement, file operations, keybinding management, and text
 * manipulation
 */

#include "../include/builtins.h"
#include "../include/macro.h"
#include "../include/async_process.h"
#include "../include/append_buffer.h"
#include "../include/os.h"
#include "../include/buffer.h"
#include "../include/daemon.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/file_finder.h"
#include "../include/file_io.h"
#include "../include/galloc.h"
#include "../include/input.h"
#include "../include/memory_inspector.h"
#include "../include/output.h"
#include "../include/project_search.h"
#include "../include/replace.h"
#include "../include/split_screen.h"
#include "../include/subprocess.h"
#include "../include/syntax_highlighter.h"
#include "../include/terminal.h"
#include "../include/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/buffer_switcher.h"
#include "../include/completion.h"
#include "../include/config_load.h"
#include "../include/init.h"
#include "../include/list_popup.h"
#include "../include/utf8.h"
#include "../include/which_key.h"

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
struct prefix_t find_prefix(const char *prefix_name) {
  int i = E.number_of_prefix + 1;
  while (i--) {
    if (!strcmp(prefix_name, E.prefix[i].prefix_name)) {
      return E.prefix[i];
    }
  }
  return E.prefix[0];
}

/**
 * @brief The 4th argument of map-key, as " ext1 ext2 " (NULL for none)
 * @details Accepts one extension string ("py") or a list of them
 * ('("c" "h")); a leading '.' is dropped.
 */
static char *keyBindExtensions(Lisp arg) {
  size_t cap = 2;
  size_t len;
  char *out;
  const char *ext;
  Lisp it;

  if (lisp_is_null(arg))
    return NULL;
  for (it = arg; lisp_is_pair(it); it = lisp_cdr(it))
    cap += strlen(lisp_string(lisp_car(it))) + 1;
  if (!lisp_is_pair(arg))
    cap += strlen(lisp_string(arg)) + 1;
  out = galloc(cap);
  if (!out)
    return NULL;
  strcpy(out, " ");
  for (it = arg; !lisp_is_null(it);) {
    ext = lisp_is_pair(it) ? lisp_string(lisp_car(it)) : lisp_string(it);
    if (*ext == '.')
      ext++;
    len = strlen(out);
    snprintf(out + len, cap - len, "%s ", ext);
    if (!lisp_is_pair(it))
      break;
    it = lisp_cdr(it);
  }
  return out;
}

static int sameExtensions(const char *a, const char *b) {
  if (!a || !b)
    return a == b;
  return strcmp(a, b) == 0;
}

/**
 * @brief Lisp function to bind a command to a key sequence
 * @details Registers a keybinding with an associated Lisp command and optional
 * prefix modifier. Dynamically extends the keybind array.
 * @param args Lisp list of 3 or 4 arguments:
 *        (key-sequence command prefix-name [extensions])
 *        - key_sequence (string): The key or key combination to bind
 *        - command (Lisp): The Lisp command/function to execute
 *        - prefix_name (string): Prefix modifier name
 *        - extensions (optional, string or list of strings): only bind the
 *          key in buffers whose file has one of these extensions, e.g.
 *          "py" or '("c" "h"). Such a binding wins over a global one there,
 *          so every language package can bind the same key (CTRL-d).
 * @param e Error pointer for Lisp error handling
 * @param ctx Lisp context
 * @return lisp_null()
 * @note Updates global editor state E (key_binds array)
 */
Lisp mapKey(Lisp args, LispError *e, const LispContext ctx) {
  const char *key_sequence = lisp_string(lisp_car(args));
  int i;
  void *memory_temp;
  Lisp func;
  const char *prefix_name;
  struct prefix_t prefix;
  char *extensions;
  size_t key_len;
  args = lisp_cdr(args);
  // second argument
  func = lisp_car(args);
  // Third argument
  args = lisp_cdr(args);
  prefix_name = lisp_string(lisp_car(args));
  prefix = find_prefix(prefix_name);
  // Optional fourth argument
  args = lisp_cdr(args);
  extensions = lisp_is_null(args) ? NULL : keyBindExtensions(lisp_car(args));

  // Check if key already bind

  for (i = 0; i < E.number_of_keybinds; ++i) {
    if (strcmp(E.key_binds[i].key_sequence, key_sequence) == 0 &&
        E.key_binds[i].prefix_id == prefix.prefix_id &&
        sameExtensions(E.key_binds[i].extensions, extensions)) {
      // Keybind found: the new binding replaces it. (This used to
      // write the last binding in the table instead of this one, so
      // re-binding a key silently re-bound some other key.)
      E.key_binds[i].command = func;
      gfree(extensions);
      whichKeyInvalidate();
      return lisp_null();
    }
  }

  memory_temp =
      grealloc(E.key_binds, ++E.number_of_keybinds * sizeof(struct keyBind_t));
  E.key_binds = (struct keyBind_t *)memory_temp;
  if (!E.key_binds)
    editorQuit(args, e, ctx);
  key_len = strlen(key_sequence);
  E.key_binds[E.number_of_keybinds - 1].key_sequence = (char *)galloc(key_len + 1);
  memcpy(E.key_binds[E.number_of_keybinds - 1].key_sequence, key_sequence, key_len + 1);

  E.key_binds[E.number_of_keybinds - 1].command = func;
  E.key_binds[E.number_of_keybinds - 1].prefix_id = prefix.prefix_id;
  E.key_binds[E.number_of_keybinds - 1].extensions = extensions;

  // The which-key panel caches a resolved command name per binding; adding
  // one invalidates that mapping.
  whichKeyInvalidate();

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
Lisp moveCursor(const Lisp args, LispError *e, LispContext ctx) {
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
  struct buffer_t *b;
  struct buffer_t *next;
  for (i = 0; i < E.number_of_keybinds; ++i) {
    gfree(E.key_binds[i].key_sequence);
    E.key_binds[i].key_sequence = NULL;
    gfree(E.key_binds[i].extensions);
    E.key_binds[i].extensions = NULL;
  }
  gfree(E.key_binds);
  E.key_binds = NULL;
  E.number_of_keybinds = 0;

  gfree(E.hooks);
  E.hooks = NULL;
  E.number_of_hooks = 0;

  // which-key's caches are keyed off the tables freed just above.
  whichKeyInvalidate();
  whichKeyFreeCommandList();

  // Buffers are a circular doubly-linked list now, each node individually
  // allocated — capture `next` before freeing `b` (a do-while keyed off
  // b->buffer_id would dereference already-freed memory once the loop
  // wraps back around), and use the known count rather than an ID check
  // to know when to stop.
  if (E.buffers != NULL) {
    b = E.buffers;
    for (i = 0; i < E.number_of_buffer; ++i) {
      next = b->next;
      bufferFree(b);
      b = next;
    }
  }
  E.buffers = NULL;
  E.number_of_buffer = 0;

  // (lsp-server ...) registrations: the table, and each argv string
  // (utilStrdup — plain malloc, which gfree hands back to gfree()).
  for (i = 0; i < E.number_of_lsp_servers; ++i)
    for (j = 0; j < LSP_SERVER_MAX_ARGS && E.lsp_servers[i].argv[j]; ++j)
      gfree(E.lsp_servers[i].argv[j]);
  gfree(E.lsp_servers);
  E.lsp_servers = NULL;
  E.number_of_lsp_servers = 0;

  editorFreeScreenCache();
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
  // In the daemon this closes the window it was typed in, and the buffers
  // stay open in the daemon: nothing unsaved is lost, so no warning.
  if (daemonIsRunning()) {
    E.exit_requested = 1;
    return lisp_null();
  }
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
 * @brief (editor-split-screen-vertical) — splits the active pane
 * left/right.
 * @details Used to rebuild the layout as exactly two panes showing
 * E.buffers and E.buffers->next, which capped the editor at two panes and
 * needed two buffers open to do anything at all. It now subdivides
 * whichever pane is active, so it composes to any number of panes and
 * works with a single buffer open — the new pane starts on the same
 * buffer, and CTRL-x ARROW-RIGHT / CTRL-x b changes it.
 */
Lisp l_editorSplitScreenVertical(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  splitScreenSplitActive(SPLIT_VERTICAL);
  return lisp_null();
}

/**
 * @brief (editor-split-screen-horizontal) — splits the active pane
 * top/bottom.
 * @details The horizontal counterpart existed in split_screen.c from the
 * start but was never registered as a builtin, so there was no way to
 * reach it from a keybind or M-x — the screen could only ever be split
 * one way.
 */
Lisp l_editorSplitScreenHorizontal(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  splitScreenSplitActive(SPLIT_HORIZONTAL);
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
  int i;
  *space.character = ' ';
  space.length = 1;
  for (i = 0; i < E.constantes.TAB_LENGTH; ++i) {
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
  const EditorPane *active = splitScreenGetActivePane();
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
  const EditorPane *active = splitScreenGetActivePane();
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
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
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
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
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
  int times;
  active->cursor_y = active->y_offset;
  times = E.screenrows;
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
  const struct buffer_t *buffer = bufferFindById(active->buffer_id);
  int times;
  active->cursor_y = active->y_offset + E.screenrows - 1;
  if (active->cursor_y > buffer->numrows) {
    active->cursor_y = buffer->numrows;
  }
  times = E.screenrows;
  while (--times) {
    editorMoveCursor(ARROW_DOWN);
  }

  return lisp_null();
}

/**
 * @brief Lisp function: generic interactive text prompt
 * @details (editor-prompt "message: %u" placeholder path-mode?) — shows
 * `message` on the status line (its one required "%u" is where the
 * in-progress input renders live, exactly like editor-open-file's
 * "Open : %u" or M-x's "M-x: %u") and returns whatever the user typed,
 * or #f if they pressed ESC. `placeholder` pre-fills the input ("" for
 * none); when `path-mode?` is true, TAB completes filesystem paths
 * against it, same as editor-open-file.
 *
 * This is the same primitive editor-open-file/execute-command/lsp-rename
 * are each already built on internally (editorPrompt() in input.c) —
 * exposed generically so a package (e.g. explorer, asking where to root
 * itself when it can't find a project) can ask its own question without
 * needing a dedicated C builtin for it.
 * @param args Lisp list of 3 arguments: (message placeholder path-mode?)
 * @return Lisp string, or #f if cancelled
 */
Lisp editorPromptLisp(Lisp args, LispError *e, LispContext ctx) {
  const char *prompt = lisp_string(lisp_car(args));
  const char *placeholder_str;
  int path_mode;
  Utf8Char *placeholder;
  Utf8Char *result;
  char *result_str;
  Lisp out;
  args = lisp_cdr(args);
  placeholder_str = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  path_mode = lisp_is_true(lisp_car(args));

  placeholder = placeholder_str[0]
                    ? stringToUtf8((unsigned char *)placeholder_str)
                    : NULL;
  result = editorPrompt((char *)prompt, placeholder, (char)path_mode);
  gfree(placeholder);

  if (!result)
    return lisp_make_bool(0); // ESC / cancelled

  result_str = (char *)utf8ToString(result);
  gfree(result);
  out = lisp_make_string2(result_str, ctx);
  gfree(result_str);
  return out;
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
  Utf8Char *pwd;
  char *slashed;
  Utf8Char *filename;
  EditorPane *active;
  char *cwd;
  appDebug("[EDITOR OPEN FILE]\n");
  // The working directory as the starting point (getcwd, not $PWD, which
  // Windows shells don't set).
  cwd = editorGetCwd();
  // With a trailing slash, so typing a name right away completes inside
  // the directory instead of appending to its name.
  slashed = utilFormat("%s%s", cwd ? cwd : "",
                       (cwd && cwd[0] && cwd[strlen(cwd) - 1] != '/' &&
                        cwd[strlen(cwd) - 1] != '\\') ? "/" : "");
  gfree(cwd);
  pwd = stringToUtf8((unsigned char *)(slashed ? slashed : ""));
  gfree(slashed);
  filename = editorPrompt("Open : %u", pwd, 1);
  if (filename) {
    active = splitScreenGetActivePane();
    active->buffer_id = bufferCreate(filename, READ_AND_WRITE, FILE_BUFF);
  }
  gfree(pwd);
  gfree(filename);

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
  Utf8Char *path = stringToUtf8((unsigned char *)E.init_file_path);
  EditorPane *active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
  gfree(path);
  return lisp_null();
}

// ─── Theme ──────────────────────────────────────────────────────────────────

/**
 * @brief Lisp function to set one theme color
 * @details (editor-set-theme-color role r g b) — role is one of
 * "background", "line-cursor-background", "keyword", "type", "string",
 * "comment", "number", "default", "added", "removed" (diff lines),
 * "debug-line-background" (where the debugger stopped); r/g/b
 * are 0-255. Writes straight into
 * E.theme, replacing whatever the fallback (or an earlier-loaded theme package)
 * set — lets a theme be an ordinary package (packages/theme-dark,
 * packages/theme-light, or your own) instead of a hardcoded C palette.
 * @param args Lisp list of 4 arguments: (role r g b)
 * @return lisp_null()
 */
Lisp editorSetThemeColor(Lisp args, LispError *e, LispContext ctx) {
  const char *role = lisp_string(lisp_car(args));
  int r;
  int g;
  int b;
  int is_background;
  char *ansi;
  char **slot;
  args = lisp_cdr(args);
  r = (int)lisp_int(lisp_car(args));
  args = lisp_cdr(args);
  g = (int)lisp_int(lisp_car(args));
  args = lisp_cdr(args);
  b = (int)lisp_int(lisp_car(args));

  is_background = (strstr(role, "background") != NULL);
  ansi = galloc(24); // "\x1b[38;2;255;255;255m" + NUL fits well within this
  snprintf(ansi, 24, "\x1b[%d;2;%d;%d;%dm", is_background ? 48 : 38, r, g, b);

  slot = !strcmp(role, "background")               ? &E.theme.BACKGROUND_COLOR
         : !strcmp(role, "line-cursor-background") ? &E.theme.LINE_CURSOR_BACKGROUND
         : !strcmp(role, "keyword")                ? &E.theme.COLOR_KEYWORD
         : !strcmp(role, "type")                   ? &E.theme.COLOR_TYPE
         : !strcmp(role, "string")                 ? &E.theme.COLOR_STRING
         : !strcmp(role, "comment")                ? &E.theme.COLOR_COMMENT
         : !strcmp(role, "number")                 ? &E.theme.COLOR_NUMBER
         : !strcmp(role, "default")                ? &E.theme.COLOR_DEFAULT
         : !strcmp(role, "added")                  ? &E.theme.COLOR_ADDED
         : !strcmp(role, "removed")                ? &E.theme.COLOR_REMOVED
         : !strcmp(role, "debug-line-background")  ? &E.theme.DEBUG_LINE_BACKGROUND
                                                   : NULL;
  if (slot) {
    // Every color is galloc'd (initEditor() copies the fallback ones), so
    // the one replaced here can be freed: a second theme, or a
    // personal-config override, no longer leaks the first.
    gfree(*slot);
    *slot = ansi;
  } else {
    gfree(ansi);
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
Lisp editorOpenFilePath(const Lisp args, LispError *e, LispContext ctx) {
  const char *path_str = lisp_string(lisp_car(args));
  Utf8Char *path = stringToUtf8((unsigned char *)path_str);
  EditorPane *active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
  gfree(path);
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
  char *cwd = editorGetCwd();
  Lisp result = lisp_make_string2(cwd ? cwd : "", ctx);
  (void)args;
  (void)e;
  gfree(cwd);
  return result;
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
Lisp listDirectory(const Lisp args, LispError *e, LispContext ctx) {
  const char *path = lisp_string(lisp_car(args));
  int count = 0;
  OsDirEntry *entries = os_list_dir(path, &count);
  Lisp result = lisp_null();
  int i;
  (void)e;
  if (!entries)
    return result;
  for (i = 0; i < count; i++)
    result = lisp_cons(lisp_cons(lisp_make_string2(entries[i].name, ctx),
                                 lisp_make_bool(entries[i].is_dir), ctx),
                       result, ctx);
  os_free_dir(entries, count);
  return result;
}

/**
 * @brief Lisp function to list every open buffer
 * @details (list-buffers) — one (label . id) pair per buffer, in the same
 * "<id>: <filename>[ *]" format and iteration order the CTRL-x b popup
 * (buffer_switcher.c) already draws, so a Lisp-driven picker built on
 * (editor-list-popup ...) + this looks identical to it — just add
 * filterable? #t for the live-search version that popup doesn't have.
 * A snapshot, unlike buffer_switcher.c's own live E.buffers walk: fine
 * here since editor-list-popup's items are captured once when it opens,
 * same as every other picker built on it.
 * @param args Lisp arguments (unused)
 * @return Lisp list of (label . id) pairs
 */
Lisp l_listBuffers(Lisp args, LispError *e, LispContext ctx) {
  Lisp head;
  Lisp tail;
  int first_buffer_id;
  struct buffer_t *b;
  char *filename;
  char line[PATH_MAX + 32];
  Lisp pair;
  Lisp cell;
  (void)args;
  (void)e;
  if (E.buffers == NULL)
    return lisp_null();

  head = lisp_null();
  tail = lisp_null();

  first_buffer_id = E.buffers->buffer_id;
  b = E.buffers;
  do {
    filename = (char *)utf8ToString(b->filename);
    snprintf(line, sizeof(line), "%d: %s%s", b->buffer_id,
             (filename && filename[0]) ? filename : "[No Name]",
             b->dirty ? " *" : "");
    gfree(filename);

    pair = lisp_cons(lisp_make_string2(line, ctx), lisp_make_int(b->buffer_id),
                     ctx);
    cell = lisp_cons(pair, lisp_null(), ctx);
    if (lisp_is_null(head))
      head = cell;
    else
      lisp_set_cdr(tail, cell);
    tail = cell;

    b = b->next;
  } while (b->buffer_id != first_buffer_id);

  return head;
}

/**
 * @brief Lisp function to switch the active pane to a specific buffer
 * @details (editor-switch-to-buffer buffer-id) — a direct, non-interactive
 * counterpart to editor-switch-buffer-popup, for Lisp code that already
 * knows which buffer it wants (e.g. after editor-scratch-open).
 * @param args Lisp list of 1 argument: (buffer-id)
 * @return lisp_null()
 */
Lisp editorSwitchToBuffer(const Lisp args, LispError *e, LispContext ctx) {
  const int buffer_id = (int)lisp_int(lisp_car(args));
  bufferSwitch(buffer_id);
  return lisp_null();
}

/**
 * @brief Lisp function to kill a buffer
 * @details (editor-kill-buffer [buffer-id]) — kills the given buffer, or
 * the active pane's one when called with no argument (CTRL-x k). See
 * bufferClose() for what "kill" covers: confirmation for unsaved files and
 * live terminals, panes switching to another buffer, the "buffer-killed"
 * hook.
 * @return #t if the buffer was killed, #f if refused or cancelled
 */
Lisp editorKillBuffer(Lisp args, LispError *e, LispContext ctx) {
  int buffer_id;
  (void)e;
  (void)ctx;
  buffer_id = lisp_is_pair(args) ? (int)lisp_int(lisp_car(args))
                                 : splitScreenGetActivePane()->buffer_id;
  return lisp_make_bool(bufferClose(buffer_id) == 0);
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
Lisp editorScratchOpen(const Lisp args, LispError *e, LispContext ctx) {
  const char *name = lisp_string(lisp_car(args));
  Utf8Char *uname = stringToUtf8((unsigned char *)name);
  const int id = bufferCreate(uname, READ_ONLY, EXPLORER_BUFF);
  gfree(uname);
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
  const int buffer_id = (int)lisp_int(lisp_car(args));
  Lisp lines;
  struct buffer_t *buf;
  int i;
  const char *line_str;
  Utf8Char *uline;
  int len;
  Utf8Char *expanded;
  int expanded_len;
  args = lisp_cdr(args);
  lines = lisp_car(args);

  buf = bufferFindById(buffer_id);
  if (!buf || buf->type != EXPLORER_BUFF)
    return lisp_null();

  for (i = 0; i < buf->numrows; i++) {
    gfree(buf->row[i].characters);
    gfree(buf->row[i].attrs);
  }
  gfree(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
  buf->x = 0;
  buf->y = 0;

  while (lisp_is_pair(lines)) {
    line_str = lisp_string(lisp_car(lines));
    uline = stringToUtf8((unsigned char *)line_str);
    len = utf8StringLen(uline) - 1; // utf8StringLen() counts the terminator too
    // Same tab expansion a file gets when it's loaded (a diff or grep hit
    // of tab-indented code used to reach the terminal as raw tabs).
    expanded = expandTabsForLine(uline, len, &expanded_len);
    if (expanded) {
      bufferInsertRow(buf, buf->numrows, expanded, expanded_len);
      gfree(expanded);
    } else {
      bufferInsertRow(buf, buf->numrows, uline, len);
    }
    gfree(uline);
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
  int width;
  EditorPane *active;
  int main_id;
  Utf8Char *uname;
  int sidebar_id;
  args = lisp_cdr(args);
  width = (int)lisp_int(lisp_car(args));

  active = splitScreenGetActivePane();
  main_id = active->buffer_id;

  // bufferFindByFilename() only decides whether the *buffer* needs
  // creating (the sidebar's tree content survives a close, same as any
  // other scratch buffer, so reopening usually finds it already there) —
  // it says nothing about whether the *split* still exists. Closing the
  // sidebar collapses back to one fullscreen pane (splitScreenCloseSidebar
  // -> splitScreenUnify), so on reopen there's no second pane to switch
  // into: splitScreenWithSidebar() has to run every time, to actually
  // recreate the two-pane layout, not just on the buffer's first creation.
  // Skipping it (as this used to, in the reuse branch) left the explorer
  // buffer to bufferSwitch() into whatever single pane was active —
  // fullscreen, at that pane's stale geometry, with the previous frame's
  // gutter/content never cleared underneath it.
  uname = stringToUtf8((unsigned char *)name);
  sidebar_id = bufferFindByFilename(uname);
  if (sidebar_id == -1)
    sidebar_id = bufferCreate(uname, READ_ONLY, EXPLORER_BUFF);
  gfree(uname);

  if (sidebar_id == -1)
    return lisp_make_int(-1); // bufferCreate() itself failed

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
  EditorPane *active;
  unsigned char temp_str_file[5] = "term";
  Utf8Char *filename;
  int bufId;
  appDebug("[EDITOR OPEN TERMINAL]\n");
  active = splitScreenGetActivePane();
  filename = stringToUtf8(temp_str_file);
  bufId = bufferFindByFilename(filename);
  gfree(filename);
  if (bufId == -1)
    active->buffer_id = bufferCreate(NULL, READ_AND_WRITE, TERMINAL_BUFF);
  else
    bufferSwitch(bufId);
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
Lisp editorExecuteCommand(Lisp args, LispError *e, const LispContext ctx) {
  Utf8Char *name;
  char *name_str;
  int present;
  Lisp sym;
  Lisp val;
  LispError call_error;
  // Snapshot the callable names once, here, rather than per-frame inside the
  // help panel: building it walks the global env into an alist, which
  // allocates Lisp memory, and the panel redraws on every keystroke.
  whichKeyBuildCommandList();
  E.prompt_is_command = 1;
  name = editorPrompt("M-x: %u", NULL, 0);
  E.prompt_is_command = 0;
  whichKeyFreeCommandList();
  if (!name)
    return lisp_null();

  name_str = (char *)utf8ToString(name);
  gfree(name);

  present = 0;
  sym = lisp_make_symbol(name_str, ctx);
  // lisp_env(ctx) fetches the *current* global env fresh — the interpreter's
  // GC is a moving/compacting collector that relocates the whole heap and
  // updates ctx.p->env internally on every collection, but never touches any
  // copy of that Lisp value a caller cached earlier. A cached copy (E.env
  // used to be exactly that) goes stale the moment a single collection runs
  // — which happens once, unconditionally, at the end of lisp_lib_load(),
  // before this function is ever even reachable.
  val = lisp_env_lookup(lisp_env(ctx), sym, &present);

  if (!present) {
    editorSetStatusMessage("No such command: %s", name_str);
    gfree(name_str);
    return lisp_null();
  }

  if (lisp_type(val) != LISP_FUNC && lisp_type(val) != LISP_LAMBDA) {
    editorSetStatusMessage("Not a command: %s", name_str);
    gfree(name_str);
    return lisp_null();
  }

  call_error = LISP_ERROR_NONE;
  lisp_apply(val, lisp_null(), &call_error, ctx);
  if (call_error != LISP_ERROR_NONE)
    editorSetStatusMessage("Error running: %s", name_str);

  gfree(name_str);
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
Lisp editorPrintC(const Lisp args, LispError *e, LispContext ctx) {
  const char *src = lisp_string(lisp_car(args));
  Utf8Char ch;
  if (src && src[0]) {
    utf8CreateChar((const unsigned char *)src, &ch);
    bufferInsertChar(&ch);
  }
  return lisp_null();
}

Lisp editorSwitchNextBuffer(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active;
  struct buffer_t *current;
  appDebug("switch buffer\n");
  // buffer_id is a monotonically-increasing counter, not a dense 0..N-1
  // array index (especially once any buffer's been closed, leaving gaps),
  // so "(id + 1) % count" can land on an id that doesn't exist. Walk the
  // actual list instead.
  active = splitScreenGetActivePane();
  current = bufferFindById(active->buffer_id);
  if (current) {
    // Through bufferSwitch() rather than assigning buffer_id directly, so
    // this lands in the same keymap the buffer switcher popup would give:
    // a terminal or a scratch buffer with its own prefix needs that prefix
    // activated however you arrived at it.
    bufferSwitch(current->next->buffer_id);
  }

  return lisp_null();
}

Lisp editorSwitchPreviousBuffer(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active;
  struct buffer_t *current;
  appDebug("switch buffer\n");
  // buffer_id is a monotonically-increasing counter, not a dense 0..N-1
  // array index (especially once any buffer's been closed, leaving gaps),
  // so "(id + 1) % count" can land on an id that doesn't exist. Walk the
  // actual list instead.
  active = splitScreenGetActivePane();
  current = bufferFindById(active->buffer_id);
  if (current) {
    // Through bufferSwitch() rather than assigning buffer_id directly, so
    // this lands in the same keymap the buffer switcher popup would give:
    // a terminal or a scratch buffer with its own prefix needs that prefix
    // activated however you arrived at it.
    bufferSwitch(current->prev->buffer_id);
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
  EditorPane *active;
  struct buffer_t *buffer;
  appDebug("LispFind\n");
  active = splitScreenGetActivePane();
  buffer = bufferFindById(active->buffer_id);
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
  EditorPane *active;
  struct buffer_t *buffer;
  appDebug("LispFind\n");
  active = splitScreenGetActivePane();
  buffer = bufferFindById(active->buffer_id);
  bufferFindReverse(buffer);
  return lisp_null();
}

/**
 * @brief Lisp function to read the character under the cursor
 * @details (editor-read-char) — returns the character at the active
 * buffer's cursor position. Returns #f when there is nothing there: the
 * cursor sits at (or past) the end of the line, the row is out of range,
 * or there's no buffer. Callers test for that with a plain (if ch ...).
 *
 * A char in this dialect is one byte, so for a multi-byte UTF-8 cell this
 * hands back the lead byte — which is >= 0x80 and so reads as "not a word
 * character" to the ASCII-range predicates in init.lisp. That's the
 * intended behaviour for word motion; it is not a general text accessor.
 * @param args Lisp arguments (unused)
 * @return Lisp character, or #f
 */
Lisp editorReadChar_L(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active;
  struct buffer_t *buffer;
  Row *row;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buffer = bufferFindById(active->buffer_id);
  if (!buffer || buffer->y < 0 || buffer->y >= buffer->numrows)
    return lisp_make_bool(0);

  row = &buffer->row[buffer->y];
  if (buffer->x < 0 || buffer->x >= row->size)
    return lisp_make_bool(0);

  return lisp_make_char(row->characters[buffer->x].character[0]);
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
Lisp editorSetPrefix(const Lisp args, LispError *e, LispContext ctx) {
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
  E.prefix = (struct prefix_t *)grealloc(
      E.prefix, (++(E.number_of_prefix) + 1) * sizeof(struct prefix_t));
  E.prefix[E.number_of_prefix].prefix_id = E.number_of_prefix;
  strncpy(E.prefix[E.number_of_prefix].prefix_name, lisp_string(lisp_car(args)),
          64);
  args = lisp_cdr(args);
  E.prefix[E.number_of_prefix].sticky =
      lisp_is_pair(args) && lisp_bool(lisp_car(args));
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
  const unsigned char *p;
  Utf8Char ch;
  char *src;
  char *dst;
  const EditorPane *active;
  struct buffer_t *buf;
  if (!text)
    return lisp_null();

  // CRLF (Windows clipboards, copies from a browser) is one line break,
  // and so is a lone CR (classic Mac text).
  for (src = dst = text; *src; src++) {
    if (*src == '\r') {
      *dst++ = '\n';
      if (src[1] == '\n')
        src++;
    } else {
      *dst++ = *src;
    }
  }
  *dst = '\0';

  // One bulk insert, undone in one step. Terminal buffers keep the
  // per-key path (bufferInsertChar() decides what they accept).
  active = splitScreenGetActivePane();
  buf = active ? bufferFindById(active->buffer_id) : NULL;
  if (buf && buf->type != TERMINAL_BUFF) {
    bufferInsertText(buf, text);
    gfree(text);
    return lisp_null();
  }

  p = (const unsigned char *)text;
  while (*p) {
    if (*p == '\r') {
      // CRLF (Windows clipboards, copies from a browser) is one line break,
      // and so is a lone CR (classic Mac text).
      bufferInsertNewLine();
      p++;
      if (*p == '\n')
        p++;
    } else if (*p == '\n') {
      bufferInsertNewLine();
      p++;
    } else {
      p += utf8CreateChar(p, &ch);
      bufferInsertChar(&ch);
    }
  }

  gfree(text);
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
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  Row *row;
  int count;
  char *text;
  if (!buffer || buffer->y >= buffer->numrows)
    return lisp_null();

  row = &buffer->row[buffer->y];
  count = row->size - buffer->x;
  if (count <= 0)
    return lisp_null();

  text = (char *)utf8ToNString(&row->characters[buffer->x], count);
  editorSetClipboard(text, (int)strlen(text));
  gfree(text);

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
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  Row *row;
  int count;
  char *text;
  if (!buffer || buffer->y >= buffer->numrows)
    return lisp_null();

  row = &buffer->row[buffer->y];
  count = row->size - buffer->x;
  if (count <= 0)
    return lisp_null();

  text = (char *)utf8ToNString(&row->characters[buffer->x], count);
  editorSetClipboard(text, (int)strlen(text));
  gfree(text);

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
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
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
  EditorPane *active;
  struct buffer_t *buf;
  int sr;
  int sc;
  int er;
  int ec;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf || !buf->mark_active) {
    editorSetStatusMessage("No region (CTRL-x m to set the mark first)");
    return lisp_null();
  }

  bufferRegionBounds(buf, &sr, &sc, &er, &ec);
  bufferDeleteRegion(buf, sr, sc, er, ec);
  buf->mark_active = 0;
  buf->b_has_changed = 1;
  return lisp_null();
}

/**
 * @brief Lisp function to cut the active region to the clipboard.
 * @details No-op with a status message if no mark is active. Clears the
 * mark afterward, same as real Emacs' C-w.
 * @return lisp_null()
 * @see editorSetMark(), editorCopyRegion()
 */
Lisp editorCutRegion(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active;
  struct buffer_t *buf;
  int sr;
  int sc;
  int er;
  int ec;
  char *text;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf || !buf->mark_active) {
    editorSetStatusMessage("No region (CTRL-x m to set the mark first)");
    return lisp_null();
  }

  bufferRegionBounds(buf, &sr, &sc, &er, &ec);
  text = bufferGetRegionText(buf, sr, sc, er, ec);
  if (text) {
    editorSetClipboard(text, (int)strlen(text));
    gfree(text);
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
  EditorPane *active;
  struct buffer_t *buf;
  int sr;
  int sc;
  int er;
  int ec;
  char *text;
  (void)args;
  (void)e;
  (void)ctx;
  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf || !buf->mark_active) {
    editorSetStatusMessage("No region (CTRL-x m to set the mark first)");
    return lisp_null();
  }

  bufferRegionBounds(buf, &sr, &sc, &er, &ec);
  text = bufferGetRegionText(buf, sr, sc, er, ec);
  if (text) {
    editorSetClipboard(text, (int)strlen(text));
    gfree(text);
  }
  buf->mark_active = 0;
  return lisp_null();
}

Lisp editorMoveBegBuffer(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  buffer->x = 0;
  buffer->y = 0;
  editorGetPaneHeight(lisp_null(), &E.ctx_error, E.ctx);
  return lisp_null();
}

Lisp editorMoveEndBuffer(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  buffer->x = buffer->row[buffer->numrows - 1].size;
  buffer->y = buffer->numrows - 1;
  return lisp_null();
}

Lisp editorAutoComplete(Lisp args, LispError *e, LispContext ctx) {
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buffer;
  if (!E.constantes.LSP) {
    return lisp_null();
  }
  appDebug("editor-auto-complete\n");
  active = splitScreenGetActivePane();
  buffer = bufferFindById(active->buffer_id);
  lspRequestCompletion(lsp, buffer, buffer->y, buffer->x,
                       active->cursor_x + active->origin_x +
                           editorGutterWidth(buffer),
                       active->cursor_y + active->origin_y);
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
  Lisp callback;
  void *mem;
  args = lisp_cdr(args);
  callback = lisp_car(args);

  mem = grealloc(E.hooks, (E.number_of_hooks + 1) * sizeof(struct hookBind_t));
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
void runHooks(const char *event_name, const Lisp args) {
  int i;
  LispError hook_error;
  for (i = 0; i < E.number_of_hooks; i++) {
    if (strcmp(E.hooks[i].event_name, event_name) != 0)
      continue;
    hook_error = LISP_ERROR_NONE;
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
Lisp lineTextAt(const Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  int row;
  char *text;
  Lisp result;
  if (!buf)
    return lisp_make_string2("", ctx);

  row = lisp_is_pair(args) ? (int)lisp_int(lisp_car(args)) : buf->y;
  if (row < 0 || row >= buf->numrows)
    return lisp_make_string2("", ctx);

  text = (char *)utf8ToString(buf->row[row].characters);
  result = lisp_make_string2(text, ctx);
  gfree(text);
  return result;
}

/**
 * @brief Lisp function returning the cursor's 0-based row in the active buffer
 */
Lisp cursorRowLisp(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  const struct buffer_t *buf = bufferFindById(active->buffer_id);
  return lisp_make_int(buf ? buf->y : 0);
}

/**
 * @brief Lisp function returning the cursor's 0-based column in the active
 * buffer
 */
Lisp cursorColLisp(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  const struct buffer_t *buf = bufferFindById(active->buffer_id);
  return lisp_make_int(buf ? buf->x : 0);
}

/**
 * @brief Lisp function returning the active buffer's row count
 */
Lisp bufferLineCountLisp(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  const struct buffer_t *buf = bufferFindById(active->buffer_id);
  return lisp_make_int(buf ? buf->numrows : 0);
}

/**
 * @brief Lisp function returning the active buffer's absolute file path
 * @details Empty string for a buffer with no backing file (e.g. a
 * terminal buffer never gets a real path here).
 */
Lisp bufferFilenameLisp(Lisp args, LispError *e, const LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  char *text;
  Lisp result;
  if (!buf || !buf->fullname)
    return lisp_make_string2("", ctx);

  text = (char *)utf8ToString(buf->fullname);
  result = lisp_make_string2(text, ctx);
  gfree(text);
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
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  int row;
  int col;
  if (!buffer || buffer->numrows == 0)
    return lisp_null();

  row = (int)lisp_int(lisp_car(args));
  if (row < 0)
    row = 0;
  if (row >= buffer->numrows)
    row = buffer->numrows - 1;

  args = lisp_cdr(args);
  col = lisp_is_pair(args) ? (int)lisp_int(lisp_car(args)) : 0;
  if (col < 0)
    col = 0;
  if (col > buffer->row[row].size)
    col = buffer->row[row].size;

  buffer->y = row;
  buffer->x = col;
  return lisp_null();
}

/**
 * @brief Lisp function to open a generic modal list picker
 * @details (editor-list-popup title items on-select [on-cancel] [filterable?])
 * — a primitive any Lisp package can use to show a selectable list
 * without writing its own C popup. `items` is a list of (label . value)
 * pairs where label is a string; `on-select` is called with the chosen
 * pair's cdr when the user presses ENTER, `on-cancel` (optional, pass
 * '() for none) with no arguments if they press ESC instead.
 *
 * `filterable?` (optional, defaults to #f) turns on live filtering: any
 * printed key and BACKSPACE edit a query shown in the title bar, and the
 * list narrows to labels containing it (case-insensitive substring) as
 * you type — see find-file (CTRL-P) for what that feels like from the
 * user's side, and list_popup.c's own doc comment for when this is (and
 * isn't) the right primitive to reach for. Omitting it, or every existing
 * 3- and 4-argument caller (outline, latex-insert-environment, ...),
 * keeps the old modal-arrows-only behavior exactly as it was.
 * @param args Lisp list of 3-5 arguments: (title items on-select [on-cancel]
 * [filterable?])
 * @return lisp_null()
 */
Lisp editorListPopup(Lisp args, LispError *e, LispContext ctx) {
  const char *title = lisp_string(lisp_car(args));
  Lisp items;
  Lisp on_select;
  Lisp on_cancel;
  int filterable;
  args = lisp_cdr(args);
  items = lisp_car(args);
  args = lisp_cdr(args);
  on_select = lisp_car(args);
  args = lisp_cdr(args);
  on_cancel = lisp_is_pair(args) ? lisp_car(args) : lisp_null();
  if (lisp_is_pair(args))
    args = lisp_cdr(args);
  filterable = lisp_is_pair(args) && lisp_is_true(lisp_car(args));

  listPopupOpen(title, items, on_select, on_cancel, filterable);
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
  const char *lang_id;
  const char *command;
  void *mem;
  struct lspServerConfig_t *cfg;
  int argc;
  int i;
  args = lisp_cdr(args);
  lang_id = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  command = lisp_is_null(args) ? "" : lisp_string(lisp_car(args));

  // The first server registered for an extension is the one used. Two
  // packages claiming the same extension (nodejs and angular both take
  // "ts") used to fail silently: say which one won.
  for (i = 0; i < E.number_of_lsp_servers; i++) {
    cfg = &E.lsp_servers[i];
    if (strcmp(cfg->extension, ext) != 0)
      continue;
    if (cfg->argv[0] && strcmp(cfg->argv[0], command) != 0) {
      editorSetStatusMessage("lsp-server: .%s already uses %s, ignoring %s "
                             "(two packages claim .%s)",
                             ext, cfg->argv[0], command, ext);
      appDebug("lsp-server: .%s already registered with %s; ignoring %s",
               ext, cfg->argv[0], command);
    }
    return lisp_null();
  }

  mem = grealloc(E.lsp_servers, (E.number_of_lsp_servers + 1) *
                                    sizeof(struct lspServerConfig_t));
  if (!mem)
    return lisp_null();
  E.lsp_servers = (struct lspServerConfig_t *)mem;

  cfg = &E.lsp_servers[E.number_of_lsp_servers];
  memset(cfg, 0, sizeof(*cfg));
  strncpy(cfg->extension, ext, sizeof(cfg->extension) - 1);
  strncpy(cfg->language_id, lang_id, sizeof(cfg->language_id) - 1);

  argc = 0;
  while (!lisp_is_null(args) && argc < LSP_SERVER_MAX_ARGS) {
    cfg->argv[argc] = utilStrdup(lisp_string(lisp_car(args)));
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
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY)
    return lisp_null();

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  lspRequestDefinition(lsp, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function to list the references to the symbol at the cursor
 * @details (lsp-references) — asks the server (async); the answer is shown
 * in the "*search*" buffer, grouped by file, ENTER jumps to one.
 * @return lisp_null()
 */
Lisp lspReferences(Lisp args, LispError *e, LispContext ctx) {
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  lspRequestReferences(lsp, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function to list the symbols of the current buffer
 * @details (lsp-document-symbols) — asks the server (blocking up to 2s)
 * and returns one (label . row) pair per symbol, in file order, a struct's
 * fields indented under it: the shape (editor-list-popup ...) takes, so
 * the outline package can show them as they are. '() when there's no
 * server for this buffer or it didn't answer, so callers can fall back.
 * @return The list, or lisp_null()
 */
Lisp lspDocumentSymbols(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = active ? bufferFindById(active->buffer_id) : NULL;
  LspClient *lsp = buf ? lspClientForBuffer(buf) : NULL;
  LspSymbol *symbols;
  Lisp head;
  Lisp tail;
  Lisp cell;
  char label[512];
  int n;
  int i;
  (void)args;
  (void)e;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY ||
      buf->type != FILE_BUFF)
    return lisp_null();

  n = lspRequestDocumentSymbols(lsp, buf, &symbols);
  head = lisp_null();
  tail = lisp_null();
  for (i = 0; i < n; i++) {
    snprintf(label, sizeof(label), "%d: %*s%s %s", symbols[i].line + 1,
             symbols[i].depth * 2, "", lspSymbolKindName(symbols[i].kind),
             symbols[i].name);
    cell = lisp_cons(lisp_cons(lisp_make_string2(label, ctx),
                               lisp_make_int(symbols[i].line), ctx),
                     lisp_null(), ctx);
    if (lisp_is_null(head))
      head = cell;
    else
      lisp_set_cdr(tail, cell);
    tail = cell;
  }
  if (n > 0)
    lspFreeSymbols(symbols, n);
  return head;
}

/**
 * @brief Lisp function to search the project's symbols by name
 * @details (lsp-workspace-symbols [query]) — prompts for the name when no
 * query is given, then asks the server (async); the matches are listed in
 * the "*search*" buffer, ENTER jumps to one.
 * @return lisp_null()
 */
Lisp lspWorkspaceSymbols(Lisp args, LispError *e, LispContext ctx) {
  LspClient *lsp = lspActiveClient();
  Utf8Char *input;
  char *query;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }

  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING) {
    lspRequestWorkspaceSymbols(lsp, lisp_string(lisp_car(args)));
    return lisp_null();
  }

  input = editorPrompt("Symbol: %s (ESC to cancel)", NULL, 0);
  if (!input)
    return lisp_null();
  query = (char *)utf8ToString(input);
  gfree(input);
  if (query && query[0] != '\0')
    lspRequestWorkspaceSymbols(lsp, query);
  gfree(query);
  return lisp_null();
}

/**
 * @brief Lisp function to show what the macro under the cursor expands to
 * @details (lsp-expand-macro) — its definition and expansion in the hover
 * popup. Waits for the server (up to 1.5s).
 * @return lisp_null()
 */
Lisp lspExpandMacro(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = active ? bufferFindById(active->buffer_id) : NULL;
  LspClient *lsp = buf ? lspClientForBuffer(buf) : NULL;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }
  lspShowMacroExpansion(lsp, buf);
  return lisp_null();
}

/**
 * @brief Lisp function to show or hide the memory layout of a struct
 * @details (lsp-struct-layout) — after each field of the struct around the
 * cursor, its offset, size and the padding after it; after the struct's
 * name, its size and total padding. Again (or an edit) hides them.
 * @return lisp_null()
 */
Lisp lspStructLayout(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = active ? bufferFindById(active->buffer_id) : NULL;
  LspClient *lsp = buf ? lspClientForBuffer(buf) : NULL;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }
  lspToggleStructLayout(lsp, buf);
  return lisp_null();
}

/**
 * @brief Lisp function to list who calls the function at the cursor
 * @details (lsp-incoming-calls [depth]) — the function, then its callers
 * indented under it, theirs under them, `depth` levels (default 3, at
 * most 5): a list of (label . (path row col)) pairs, (path row col) being
 * where the call is — the shape (editor-list-popup ...) and
 * (lsp-goto-location ...) take. '() when there's nothing to show.
 * @return The list, or lisp_null()
 */
Lisp lspIncomingCalls(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = active ? bufferFindById(active->buffer_id) : NULL;
  LspClient *lsp = buf ? lspClientForBuffer(buf) : NULL;
  LspCallNode *nodes;
  Lisp head;
  Lisp tail;
  Lisp cell;
  Lisp loc;
  int depth = 3;
  int n;
  int i;
  (void)e;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY)
    return lisp_null();
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT)
    depth = (int)lisp_int(lisp_car(args));

  n = lspIncomingCallTree(lsp, buf, depth, &nodes);
  head = lisp_null();
  tail = lisp_null();
  for (i = 0; i < n; i++) {
    loc = lisp_cons(lisp_make_string2(nodes[i].path, ctx),
                    lisp_cons(lisp_make_int(nodes[i].line),
                              lisp_cons(lisp_make_int(nodes[i].col), lisp_null(), ctx),
                              ctx),
                    ctx);
    cell = lisp_cons(lisp_cons(lisp_make_string2(nodes[i].label, ctx), loc, ctx),
                     lisp_null(), ctx);
    if (lisp_is_null(head))
      head = cell;
    else
      lisp_set_cdr(tail, cell);
    tail = cell;
  }
  if (n > 0)
    lspFreeCallTree(nodes, n);
  return head;
}

/**
 * @brief Lisp function to open a file at a position
 * @details (lsp-goto-location (path row col)) — switches to the file's
 * buffer (opening it if needed) with the cursor at 0-based row/col: what
 * an item of (lsp-incoming-calls) carries.
 * @return lisp_null()
 */
Lisp lspGotoLocationBuiltin(Lisp args, LispError *e, LispContext ctx) {
  Lisp loc = lisp_car(args);
  (void)e;
  (void)ctx;
  if (!lisp_is_pair(loc) || lisp_type(lisp_car(loc)) != LISP_STRING ||
      !lisp_is_pair(lisp_cdr(loc)) || !lisp_is_pair(lisp_cdr(lisp_cdr(loc))))
    return lisp_null();
  lspGotoLocation(lisp_string(lisp_car(loc)), (int)lisp_int(lisp_car(lisp_cdr(loc))),
                  (int)lisp_int(lisp_car(lisp_cdr(lisp_cdr(loc)))));
  return lisp_null();
}

/**
 * @brief Lisp function to list every diagnostic of the open buffers
 * @details (lsp-diagnostics) — in the "*search*" buffer, ENTER jumps to one.
 * @return lisp_null()
 */
Lisp lspDiagnostics(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  lspShowDiagnosticsList();
  return lisp_null();
}

/**
 * @brief Lisp function to show hover info (type/doc) for the symbol at the
 * cursor in the status bar. Async: see lsp_on_hover_response().
 * @return lisp_null()
 */
Lisp lspHover(Lisp args, LispError *e, LispContext ctx) {
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY)
    return lisp_null();

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  lspRequestHover(lsp, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function to show the signature of the call the cursor is in,
 * with the current argument highlighted, in a popup above the cursor.
 * Async: see lsp_on_signature_help_response(). Also fired automatically on
 * "(" and "," unless (define LSP-SIGNATURE-HELP #f).
 * @return lisp_null()
 */
Lisp lspSignatureHelp(Lisp args, LispError *e, LispContext ctx) {
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY)
    return lisp_null();

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  lspRequestSignatureHelp(lsp, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function to list the server's code actions at the cursor —
 * quick fixes for its line's diagnostics first, then refactorings — in a
 * popup; ENTER applies the chosen one (see lsp-apply-code-action).
 * @return lisp_null()
 */
Lisp lspCodeAction(Lisp args, LispError *e, LispContext ctx) {
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf || buf->type != FILE_BUFF)
    return lisp_null();

  lspRequestCodeAction(lsp, buf, buf->y, buf->x);
  return lisp_null();
}

/**
 * @brief Lisp function applying entry `index` of the last code-action
 * list — the code-action popup's on-select callback.
 * @param args Lisp list of 1 argument: (index)
 * @return lisp_null()
 */
Lisp lspApplyCodeAction_L(Lisp args, LispError *e, LispContext ctx) {
  (void)e;
  (void)ctx;
  if (!lisp_is_pair(args))
    return lisp_null();
  // NULL: the server that offered the actions (see lspApplyCodeAction()).
  lspApplyCodeAction(NULL, (int)lisp_int(lisp_car(args)));
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
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  Utf8Char *name_input;
  char *new_name;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY) {
    editorSetStatusMessage("LSP not ready");
    return lisp_null();
  }

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf)
    return lisp_null();

  name_input = editorPrompt("New name: %s (ESC to cancel)", NULL, 0);
  if (!name_input)
    return lisp_null();

  new_name = (char *)utf8ToString(name_input);
  gfree(name_input);
  if (new_name[0] == '\0') {
    gfree(new_name);
    return lisp_null();
  }

  lspRequestRename(lsp, buf, buf->y, buf->x, new_name);
  gfree(new_name);
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
  LspClient *lsp = lspActiveClient();
  EditorPane *active;
  struct buffer_t *buf;
  (void)args;
  (void)e;
  (void)ctx;
  if (!E.constantes.LSP || !lsp || lsp->state != LSP_READY)
    return lisp_null();

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);
  if (!buf || buf->type != FILE_BUFF)
    return lisp_null();

  lspRequestFormatting(lsp, buf);
  return lisp_null();
}

/**
 * @brief Lisp function to search the whole project for a literal string
 * @details (project-search) prompts for the text, greps from the working
 * directory down (git grep inside a repo, plain grep -r otherwise), and
 * shows the hits in a "*search*" buffer grouped by file. See
 * project_search.c for what runs and why.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp l_projectSearch(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  projectSearchPrompt();
  return lisp_null();
}

/**
 * @brief Lisp function: query-replace in the current buffer
 * @details (query-replace) prompts for the text and its replacement, then
 * asks at each match from the cursor on. See replace.c.
 * @return lisp_null()
 */
Lisp l_queryReplace(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  queryReplacePrompt();
  return lisp_null();
}

/**
 * @brief Lisp function: query-replace across the project
 * @details (project-query-replace) — same, over every file project-search
 * would find the text in. Buffers are left unsaved. See replace.c.
 * @return lisp_null()
 */
Lisp l_projectQueryReplace(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  projectQueryReplacePrompt();
  return lisp_null();
}

/**
 * @brief Lisp function to jump to the search result under the cursor
 * @details (project-search-goto) — bound to ENTER under the "search"
 * prefix, which the results buffer activates when it gets focus. Opens
 * the hit's file in the other pane when the screen is split, keeping the
 * result list visible; in place otherwise.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp l_projectSearchGoto(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  projectSearchGotoCursor();
  return lisp_null();
}

/**
 * @brief Lisp function to open the fuzzy project-file picker
 * @details (find-file [dir]) — pops up a modal list of every file in the
 * project (git ls-files inside a repo, a pruned find(1) otherwise, rooted
 * at `dir`, or else the current project, or else the working directory),
 * narrows it as you type, and opens the highlighted file into the active
 * pane on ENTER (reusing an already-open buffer for that path rather than
 * stacking a duplicate). ESC cancels. See file_finder.c.
 * @param args Lisp list of 0 or 1 arguments: ([dir])
 * @return lisp_null()
 */
Lisp l_findFile(Lisp args, LispError *e, LispContext ctx) {
  (void)e;
  (void)ctx;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING &&
      lisp_string(lisp_car(args))[0] != '\0')
    fileFinderOpenAt(lisp_string(lisp_car(args)));
  else
    fileFinderOpen();
  return lisp_null();
}

/**
 * @brief Lisp predicate reporting whether this binary was built with
 * APP_DEBUG (install.sh --debug)
 * @details (app-debug?) — always registered regardless of build type, so
 * config/init.lisp can gate debug-only setup (e.g. the memory inspector)
 * on it with a plain (if (app-debug?) ...) instead of referencing a
 * builtin that might not exist in a release build.
 * @param args Lisp arguments (unused)
 * @return #t under APP_DEBUG, #f otherwise
 */
Lisp l_appDebugP(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
#ifdef APP_DEBUG
  return lisp_true();
#else
  return lisp_false();
#endif
}

/**
 * @brief Lisp function: scroll so the cursor's line sits at a given screen row
 * @details (editor-recenter [row]) — like Emacs' recenter: the cursor stays
 * on the same buffer position, only the active pane's view scrolls. With no
 * argument the line goes to the middle of the pane; a row >= 0 counts from
 * the top of the pane, a negative one from the bottom (-1 = last row). The
 * top of the buffer can't scroll past row 0, so a line near the start stays
 * wherever it naturally is. Pairs with editor-goto-line for "jump there and
 * show it in context".
 * @return lisp_null()
 */
Lisp l_editorRecenter(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf;
  int target;
  (void)e;
  (void)ctx;

  if (!active)
    return lisp_null();
  buf = bufferFindById(active->buffer_id);
  if (!buf || active->height < 1)
    return lisp_null();

  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT) {
    target = (int)lisp_int(lisp_car(args));
    if (target < 0)
      target += active->height;
  } else {
    target = active->height / 2;
  }
  if (target < 0)
    target = 0;
  if (target > active->height - 1)
    target = active->height - 1;
  if (target > buf->y)
    target = buf->y; // can't scroll above the first line

  // Both halves of editorScroll()'s invariant (buf->y == cursor_y +
  // y_offset) set together, so its per-frame fixup has nothing to undo.
  active->y_offset = buf->y - target;
  active->cursor_y = target;
  return lisp_null();
}

/**
 * @brief Lisp functions: the active pane's size in text rows / columns
 * @details (pane-height) / (pane-width) — the area text is drawn in: the
 * status bar and message line aren't counted, the line-number gutter is
 * (it's inside the pane). Read them when you need them rather than caching
 * them — splits and terminal resizes change them at any time.
 * @return Lisp int
 */
Lisp l_paneHeight(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  (void)args;
  (void)e;
  (void)ctx;
  return lisp_make_int(active ? active->height : 0);
}

Lisp l_paneWidth(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  (void)args;
  (void)e;
  (void)ctx;
  return lisp_make_int(active ? active->width : 0);
}

/***********************************************************************************************************************
 *
 * Filesystem & process primitives — what a file-manager package (dired)
 * needs on top of list-directory: per-file metadata, a way to run
 * rm/mv/cp/mkdir without a shell, path expansion, and control over its
 * own buffer's keymap and the status line.
 *
 **********************************************************************************************************************/

/**
 * @brief Collapses "." / ".." / repeated slashes in an absolute path,
 * lexically — the path doesn't have to exist (a rename or mkdir target
 * usually doesn't yet), so realpath() isn't an option.
 * @param in Absolute path (starting with '/', or on Windows with a drive
 * letter: "C:/...")
 * @param out Receives the normalized path, never empty ("/" at minimum,
 * "C:/" on Windows)
 */
static void normalizePath(const char *in, char *out, const size_t out_size) {
  size_t len = 0;
  size_t root = 0; // a drive prefix ("C:") is kept, and ".." stops at it
  const char *seg = in;
  const char *end;
  size_t seg_len;

  out[0] = '\0';
  root = (size_t)os_path_drive_length(in);
  if (root > 0 && out_size > root + 1) {
    memcpy(out, in, root);
    out[root] = '\0';
    len = root;
    seg = in + root;
  }
  while (*seg) {
    while (*seg == '/')
      seg++;
    end = seg;
    while (*end && *end != '/')
      end++;
    seg_len = (size_t)(end - seg);

    if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
      // empty (trailing slash) or "." — nothing to add
    } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
      while (len > root && out[len - 1] != '/')
        len--;
      if (len > root)
        len--; // drop the slash too
      out[len] = '\0';
    } else if (len + 1 + seg_len + 1 <= out_size) {
      out[len++] = '/';
      memcpy(out + len, seg, seg_len);
      len += seg_len;
      out[len] = '\0';
    }
    seg = end;
  }
  if (len == root && out_size >= root + 2) {
    out[root] = '/';
    out[root + 1] = '\0';
  }
}

/**
 * @brief Lisp function: make a path absolute
 * @details (expand-path path [base-dir]) — expands a leading "~" to $HOME,
 * resolves a relative path against base-dir (default: the working
 * directory), and collapses "." and ".." lexically. The path doesn't have
 * to exist.
 * @return Lisp string: the absolute path
 */
Lisp l_expandPath(Lisp args, LispError *e, LispContext ctx) {
  const char *path;
  const char *base = NULL;
  char *home;
  char *cwd = NULL;
  char joined[PATH_MAX * 2];
  char out[PATH_MAX];

  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  path = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING)
    base = lisp_string(lisp_car(args));

  home = (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) ? os_home_dir() : NULL;
  if (home) {
    snprintf(joined, sizeof(joined), "%s/%s", home, path + 1);
  } else if (os_path_is_absolute(path)) {
    snprintf(joined, sizeof(joined), "%s", path);
  } else {
    if (!base || base[0] == '\0') {
      cwd = editorGetCwd();
      base = cwd ? cwd : "/";
    }
    snprintf(joined, sizeof(joined), "%s/%s", base, path);
  }
  gfree(cwd);
  gfree(home);

  os_path_to_slashes(joined); // "C:\\dir\\file" typed the Windows way
  normalizePath(joined, out, sizeof(out));
  return lisp_make_string2(out, ctx);
}

/**
 * @brief Lisp function: metadata for one path
 * @details (file-attributes path) — returns #f if the path doesn't exist,
 * otherwise the list
 *   (is-dir is-link size mtime "mode" "date" "link-target")
 * `is-link` and `mode`/`size` describe the path itself (not what a link
 * points to), while `is-dir` follows a link, so a link to a directory can
 * be entered. `mtime` is seconds since the epoch, for sorting; `date` is
 * formatted the way `ls -l` does it ("Sep 28 14:02" within the last six
 * months, "Sep 28  2025" otherwise). `link-target` is "" unless is-link.
 */
Lisp l_fileAttributes(Lisp args, LispError *e, LispContext ctx) {
  static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const char *path;
  OsFileInfo info;
  char date[32];
  time_t now;
  time_t mtime;
  const struct tm *tm;
  Lisp fields[7];

  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  path = lisp_string(lisp_car(args));
  if (os_file_info(path, &info) != 0)
    return lisp_false();

  now = time(NULL);
  mtime = (time_t)info.mtime;
  tm = localtime(&mtime);
  date[0] = '\0';
  if (tm) {
    // Same six-month cutoff ls uses to switch from time-of-day to year.
    // Built by hand: strftime's %e (space-padded day) isn't standard C.
    if (mtime > now - 15778476 && mtime <= now + 3600)
      snprintf(date, sizeof(date), "%s %2d %02d:%02d", months[tm->tm_mon % 12],
               tm->tm_mday, tm->tm_hour, tm->tm_min);
    else
      snprintf(date, sizeof(date), "%s %2d  %d", months[tm->tm_mon % 12], tm->tm_mday,
               tm->tm_year + 1900);
  }

  fields[0] = lisp_make_bool(info.is_dir);
  fields[1] = lisp_make_bool(info.is_link);
  fields[2] = lisp_make_int((LispInt)info.size);
  fields[3] = lisp_make_int((LispInt)info.mtime);
  fields[4] = lisp_make_string2(info.mode, ctx);
  fields[5] = lisp_make_string2(date, ctx);
  fields[6] = lisp_make_string2(info.link_target, ctx);
  return lisp_make_list2(fields, 7, ctx);
}

/**
 * @brief Lisp function: run a program and capture its output
 * @details (process-run "program" "arg" ...) — runs the program directly
 * (searched on PATH), **not** through a shell: every argument reaches it
 * verbatim, so file names with spaces or quotes need no escaping. stdout
 * and stderr are captured together; stdin is empty.
 * Blocks the editor until the program exits, up to 60 seconds (then it's
 * killed), keeping at most 1 MiB of output. A number before the program,
 * (process-run 300000 "git" "clone" ...), is a longer (or shorter) limit
 * in milliseconds.
 * @return (exit-code . "output"): exit-code 0 on success, 127 if the
 * program couldn't be run, -1 if it was killed; output with trailing
 * newlines stripped, "" if none.
 */
/**
 * @brief Lisp function: which platform the editor runs on
 * @details (system-type) — "windows", "darwin" (macOS), "linux", or
 * "unix" for anything else.
 * @return Lisp string
 */
Lisp l_systemType(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  return lisp_make_string2(os_name(), ctx);
}

/**
 * @brief Lisp function: quote a string for shell-run
 * @details (shell-quote "it's") — one literal word for the platform's
 * shell: 'it'\''s' for sh, 'it''s' for PowerShell.
 * @return Lisp string
 */
Lisp l_shellQuote(Lisp args, LispError *e, LispContext ctx) {
  char *quoted;
  Lisp result;
  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  quoted = os_shell_quote(lisp_string(lisp_car(args)));
  if (!quoted)
    return lisp_null();
  result = lisp_make_string2(quoted, ctx);
  gfree(quoted);
  return result;
}

/**
 * @brief Lisp function: run a shell command and capture its output
 * @details (shell-run "command" [dir]) — like process-run, but through the
 * platform's shell: /bin/sh -c on Unix, PowerShell on Windows (pwsh if
 * installed), from `dir` if given. Same contract: stdout and stderr
 * together, stdin empty, a 60s deadline, and (exit-code . "output").
 * @return Lisp pair (exit-code . output)
 */
Lisp l_shellRun(Lisp args, LispError *e, LispContext ctx) {
  const char *command;
  const char *dir = NULL;
  char **argv;
  char *out;
  int status;
  Lisp result;

  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  command = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING)
    dir = lisp_string(lisp_car(args));

  argv = os_shell_argv(command);
  if (!argv)
    return lisp_null();
  out = runArgvCaptureIn(argv, dir, 60000, 1 << 20, &status);
  os_free_argv(argv);

  result = lisp_cons(lisp_make_int(status), lisp_make_string2(out ? out : "", ctx), ctx);
  gfree(out);
  return result;
}

/**
 * @brief (code . message) for the file-operation builtins below — the
 * same shape as process-run's (exit-code . output): 0 and "" on success.
 */
static Lisp fileOpResult(const int rc, const char *message, LispContext ctx) {
  return lisp_cons(lisp_make_int(rc == 0 ? 0 : 1), lisp_make_string2(rc == 0 ? "" : message, ctx),
                   ctx);
}

/**
 * @brief The strings of a Lisp list, as a galloc'd NULL-terminated array
 * (NULL on a non-string element).
 */
static const char **stringList(Lisp list, int *count) {
  Lisp it;
  const char **out;
  int n = 0;
  for (it = list; lisp_is_pair(it); it = lisp_cdr(it)) {
    if (lisp_type(lisp_car(it)) != LISP_STRING)
      return NULL;
    n++;
  }
  out = galloc((size_t)(n + 1) * sizeof(char *));
  if (!out)
    return NULL;
  n = 0;
  for (it = list; lisp_is_pair(it); it = lisp_cdr(it))
    out[n++] = lisp_string(lisp_car(it));
  out[n] = NULL;
  *count = n;
  return out;
}

/**
 * @brief Lisp function: delete a file or a directory tree
 * @details (delete-path "path") — permanently, like rm -rf; a symbolic
 * link is removed, not followed. Returns (0 . "") or (1 . "reason").
 */
Lisp l_deletePath(Lisp args, LispError *e, LispContext ctx) {
  char err[512];
  int rc;
  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  rc = os_delete_tree(lisp_string(lisp_car(args)), err, sizeof(err));
  return fileOpResult(rc, err, ctx);
}

/**
 * @brief move-paths / copy-paths: (op '("src" ...) "dest").
 */
static Lisp transferPaths(const int move, Lisp args, LispError *e, LispContext ctx) {
  const char **paths;
  const char *dest;
  char err[512];
  int count = 0;
  int rc;
  if (!lisp_is_pair(args) || !lisp_is_pair(lisp_cdr(args)) ||
      lisp_type(lisp_car(lisp_cdr(args))) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  paths = stringList(lisp_car(args), &count);
  if (!paths) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  dest = lisp_string(lisp_car(lisp_cdr(args)));
  rc = move ? os_move(paths, count, dest, err, sizeof(err))
            : os_copy_tree(paths, count, dest, err, sizeof(err));
  gfree(paths);
  return fileOpResult(rc, err, ctx);
}

/**
 * @brief Lisp function: move or rename, like mv
 * @details (move-paths '("a" "b") "dest") — into dest when it's an
 * existing directory, else (one source) renamed to dest.
 */
Lisp l_movePaths(Lisp args, LispError *e, LispContext ctx) {
  return transferPaths(1, args, e, ctx);
}

/**
 * @brief Lisp function: copy, like cp -R
 * @details (copy-paths '("a" "dir") "dest") — directories recursively.
 */
Lisp l_copyPaths(Lisp args, LispError *e, LispContext ctx) {
  return transferPaths(0, args, e, ctx);
}

/**
 * @brief Lisp function: create a directory and its parents
 * @details (make-directories "a/b/c") — like mkdir -p.
 */
Lisp l_makeDirectories(Lisp args, LispError *e, LispContext ctx) {
  char err[512];
  int rc;
  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  rc = os_mkdirs(lisp_string(lisp_car(args)), err, sizeof(err));
  return fileOpResult(rc, err, ctx);
}

Lisp l_processRun(Lisp args, LispError *e, LispContext ctx) {
  Lisp it;
  int argc = 0;
  int i;
  char **argv;
  char *out;
  int status;
  long timeout_ms = 60000;
  Lisp result;

  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT) {
    timeout_ms = (long)lisp_int(lisp_car(args));
    args = lisp_cdr(args);
  }
  for (it = args; lisp_is_pair(it); it = lisp_cdr(it)) {
    if (lisp_type(lisp_car(it)) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    argc++;
  }
  if (argc == 0) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }

  argv = galloc((size_t)(argc + 1) * sizeof(char *));
  if (!argv)
    return lisp_null();
  i = 0;
  for (it = args; lisp_is_pair(it); it = lisp_cdr(it))
    argv[i++] = (char *)lisp_string(lisp_car(it));
  argv[argc] = NULL;

  out = runArgvCapture(argv, timeout_ms, 1 << 20, &status);
  gfree(argv);

  result = lisp_cons(lisp_make_int(status),
                     lisp_make_string2(out ? out : "", ctx), ctx);
  gfree(out);
  return result;
}

/**
 * @brief Lisp function: run a program in the background
 * @details (process-start callback "program" "arg" ...) — like process-run
 * (no shell, stdout+stderr captured, stdin empty), but returns at once;
 * `callback` is called later, from the main loop, with the same
 * (exit-code . "output") pair once the program exits. The program has no
 * controlling terminal, so anything that would prompt for a password fails
 * instead (see async_process.h). At most 16 run at a time.
 * @return the process id (>= 0), or #f if it couldn't be started
 */
Lisp l_processStart(Lisp args, LispError *e, LispContext ctx) {
  Lisp callback;
  Lisp it;
  int argc = 0;
  int i;
  char **argv;
  int id;
  (void)ctx;

  if (!lisp_is_pair(args)) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  callback = lisp_car(args);
  args = lisp_cdr(args);
  for (it = args; lisp_is_pair(it); it = lisp_cdr(it)) {
    if (lisp_type(lisp_car(it)) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    argc++;
  }
  if (argc == 0) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }

  argv = galloc((size_t)(argc + 1) * sizeof(char *));
  if (!argv)
    return lisp_false();
  i = 0;
  for (it = args; lisp_is_pair(it); it = lisp_cdr(it))
    argv[i++] = (char *)lisp_string(lisp_car(it));
  argv[argc] = NULL;

  id = asyncProcessStart(argv, callback);
  gfree(argv);
  return id >= 0 ? lisp_make_int(id) : lisp_false();
}

/**
 * @brief Lisp function: split a string on a separator
 * @details (string-split "text" "sep" [limit]) — the pieces between each
 * occurrence of `sep` (any length >= 1), empty ones included: "a,,b" on ","
 * is ("a" "" "b"). With `limit`, stops after that many pieces and drops the
 * rest — for command output that could be huge, since walking a long list
 * recursively in Lisp is bounded by the interpreter's stack.
 * @return Lisp list of strings
 */
Lisp l_stringSplit(Lisp args, LispError *e, LispContext ctx) {
  const char *text;
  const char *sep;
  size_t sep_len;
  long limit = -1;
  long count = 0;
  const char *start;
  const char *hit;
  char *piece;
  Lisp head;
  Lisp tail;
  Lisp cell;

  if (!lisp_is_pair(args) || !lisp_is_pair(lisp_cdr(args)) ||
      lisp_type(lisp_car(args)) != LISP_STRING ||
      lisp_type(lisp_car(lisp_cdr(args))) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  text = lisp_string(lisp_car(args));
  sep = lisp_string(lisp_car(lisp_cdr(args)));
  sep_len = strlen(sep);
  if (sep_len == 0) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  args = lisp_cdr(lisp_cdr(args));
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT)
    limit = (long)lisp_int(lisp_car(args));

  head = lisp_null();
  tail = lisp_null();
  start = text;
  while (limit < 0 || count < limit) {
    hit = strstr(start, sep);
    piece = hit ? utilStrndup(start, (size_t)(hit - start)) : utilStrdup(start);
    if (!piece)
      break;
    cell = lisp_cons(lisp_make_string2(piece, ctx), lisp_null(), ctx);
    gfree(piece);
    if (lisp_is_null(head))
      head = cell;
    else
      lisp_set_cdr(tail, cell);
    tail = cell;
    count++;
    if (!hit)
      break;
    start = hit + sep_len;
  }
  return head;
}

/**
 * @brief Lisp function: id of the buffer in the active pane
 * @details (current-buffer-id) — lets code that runs later (a process
 * callback, a hook) check whether its own buffer is still the one on
 * screen before moving the cursor in it.
 * @return Lisp int, or -1 with no buffer
 */
Lisp l_currentBufferId(Lisp args, LispError *e, LispContext ctx) {
  const EditorPane *active = splitScreenGetActivePane();
  (void)args;
  (void)e;
  (void)ctx;
  return lisp_make_int(active ? active->buffer_id : -1);
}

/**
 * @brief Lisp function: show a message on the status line
 * @details (editor-message "text") — the same line hover results and
 * "File saved" use; it fades like any other status message.
 * @return lisp_null()
 */
Lisp l_editorMessage(Lisp args, LispError *e, LispContext ctx) {
  (void)ctx;
  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  editorSetStatusMessage("%s", lisp_string(lisp_car(args)));
  return lisp_null();
}

/**
 * @brief Lisp function: (editor-redisplay) — draw the screen now, from
 * inside a long-running command, so a message set just before it shows.
 * @details While init.lisp is still loading there are no buffers to draw
 * yet: only the message line is drawn (the package manager's "Installing
 * …" at startup). In batch mode (beluga --sync-packages) messages go to
 * stdout as they're set, and this does nothing.
 */
Lisp l_editorRedisplay(Lisp args, LispError *e, LispContext ctx) {
  struct abuf ab = ABUF_INIT;
  (void)args;
  (void)e;
  (void)ctx;
  if (E.batch)
    return lisp_null();
  if (E.initialized) {
    editorRefreshScreen();
    return lisp_null();
  }
  editorDrawMessageBar(&ab);
  screenWrite(ab.b, (size_t)ab.len);
  abFree(&ab);
  return lisp_null();
}

/**
 * @brief Lisp function: (editor-initialized?) — #f while init.lisp (and
 * every package it loads) is being evaluated, #t from then on.
 */
Lisp l_editorInitialized(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  return lisp_make_bool(E.initialized);
}

/**
 * @brief Lisp function: (editor-batch?) — #t when beluga runs without a
 * screen (beluga --sync-packages): nothing can be asked, and background
 * processes would never report back.
 */
Lisp l_editorBatch(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  return lisp_make_bool(E.batch);
}

/**
 * @brief Lisp function: give a scratch buffer its own keymap
 * @details (editor-scratch-set-prefix buffer-id "prefix") — the prefix is
 * activated whenever that buffer gets focus (switching to it, or moving
 * into its pane), and right away if it's already the active pane's
 * buffer. Without this a scratch buffer falls back to the "explorer"
 * prefix. Only affects scratch buffers, like editor-scratch-set-lines.
 * @return #t if set, #f if buffer-id isn't a scratch buffer
 */
Lisp l_editorScratchSetPrefix(Lisp args, LispError *e, LispContext ctx) {
  int buffer_id;
  const char *prefix_name;
  struct buffer_t *buf;
  struct prefix_t prefix;
  (void)ctx;

  if (!lisp_is_pair(args) || !lisp_is_pair(lisp_cdr(args)) ||
      lisp_type(lisp_car(lisp_cdr(args))) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  buffer_id = (int)lisp_int(lisp_car(args));
  prefix_name = lisp_string(lisp_car(lisp_cdr(args)));

  buf = bufferFindById(buffer_id);
  if (!buf || buf->type != EXPLORER_BUFF)
    return lisp_false();

  snprintf(buf->focus_prefix, sizeof(buf->focus_prefix), "%s", prefix_name);
  if (splitScreenGetActivePane()->buffer_id == buffer_id) {
    prefix = find_prefix(prefix_name);
    if (prefix.prefix_id != -1)
      E.prefix_state = prefix.prefix_id;
  }
  return lisp_true();
}

#ifdef APP_DEBUG
/**
 * @brief Lisp function to open the memory inspector
 * @details (memory-inspector) — shows a "*memory*" buffer listing every
 * block currently tracked by gutils (galloc/grealloc), for spotting leaks
 * while the editor runs. Debug builds only. See memory_inspector.c.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp l_memoryInspectorOpen(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  memoryInspectorOpen();
  return lisp_null();
}

/**
 * @brief Lisp function to refresh the memory inspector in place
 * @details (memory-inspector-refresh) — bound to "r" under the "memory"
 * prefix, so the snapshot can be updated without losing scroll position.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp l_memoryInspectorRefresh(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  memoryInspectorRefresh();
  return lisp_null();
}

/**
 * @brief Lisp function to cycle the memory inspector's sort order
 * @details (memory-inspector-sort) — bound to "s" under the "memory"
 * prefix. Cycles recency -> size (largest first) -> file:line/func
 * location -> back to recency, re-rendering after each change.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp l_memoryInspectorCycleSort(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  memoryInspectorCycleSort();
  return lisp_null();
}

/**
 * @brief Lisp function to export the memory inspector's snapshot
 * @details (memory-inspector-export) — bound to "w" under the "memory"
 * prefix. Prompts for a file path (ESC cancels) and writes the currently
 * sorted snapshot to it as plain text.
 * @param args Lisp arguments (unused)
 * @return lisp_null()
 */
Lisp l_memoryInspectorExport(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  memoryInspectorExport();
  return lisp_null();
}
#endif

/**
 * @brief Finds the LanguageSyntax entry for `ext`, or NULL if none has
 * been registered yet (syntax-add-keyword/syntax-add-type before the
 * matching syntax-define-language do nothing rather than crash — same
 * "register the container first" order as add-prefix before map-key).
 */
static LanguageSyntax *findLanguageByExtension(const char *ext) {
  int i;
  for (i = 0; i < E.number_of_languages; i++)
    if (strcmp(E.languages[i].extension, ext) == 0)
      return &E.languages[i];
  return NULL;
}

/**
 * @brief Lisp function to register a language's syntax-highlighting rules
 * for a file extension
 * @details (syntax-define-language "ext" line-comment block-comment-open
 * block-comment-close quote-chars) — e.g.
 * (syntax-define-language "php" "//" "/\*" "*\/" "\"'"). Any of the four
 * strings can be "" to mean "this language doesn't have that" (YAML has
 * no block comment, so its block-open/close are both ""). Call this once
 * per extension, before any syntax-add-keyword/syntax-add-type calls for
 * it — this is what allocates the entry those look up by name.
 *
 * Re-registering an already-defined extension resets its word list back
 * to empty rather than leaving stale entries from a previous call —
 * harmless in practice (a package only calls this once for its own
 * extension), but means loading the same package twice doesn't leave a
 * duplicated keyword list behind.
 * @param args Lisp list: (extension line-comment block-open block-close
 * quote-chars)
 * @return lisp_null()
 * @note Updates global editor state E (languages array)
 */
Lisp l_syntaxDefineLanguage(Lisp args, LispError *e, LispContext ctx) {
  const char *ext = lisp_string(lisp_car(args));
  const char *line_comment;
  const char *block_open;
  const char *block_close;
  const char *quote_chars;
  LanguageSyntax *lang;
  void *mem;
  int i;
  args = lisp_cdr(args);
  line_comment = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  block_open = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  block_close = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  quote_chars = lisp_string(lisp_car(args));

  lang = findLanguageByExtension(ext);
  if (!lang) {
    mem = grealloc(E.languages, (size_t)(E.number_of_languages + 1) *
                                    sizeof(LanguageSyntax));
    if (!mem)
      return lisp_null();
    E.languages = (LanguageSyntax *)mem;
    lang = &E.languages[E.number_of_languages++];
  } else {
    gfree(lang->words); // re-registering: drop whatever keywords/types it had
  }
  memset(lang, 0, sizeof(*lang));
  strncpy(lang->extension, ext, sizeof(lang->extension) - 1);
  strncpy(lang->line_comment, line_comment, sizeof(lang->line_comment) - 1);
  strncpy(lang->block_comment_open, block_open,
          sizeof(lang->block_comment_open) - 1);
  strncpy(lang->block_comment_close, block_close,
          sizeof(lang->block_comment_close) - 1);

  lang->num_quote_chars = 0;
  for (i = 0; quote_chars[i] != '\0' &&
              lang->num_quote_chars < (int)sizeof(lang->quote_chars) - 1;
       i++)
    lang->quote_chars[lang->num_quote_chars++] = quote_chars[i];

  return lisp_null();
}

/**
 * @brief Shared body for syntax-add-keyword/syntax-add-type — appends one
 * word to `ext`'s LanguageSyntax, growing the array as needed.
 */
static void syntaxAddWord(const char *ext, const char *word,
                          const int is_type) {
  LanguageSyntax *lang = findLanguageByExtension(ext);
  int new_cap;
  void *mem;
  SyntaxWord *w;
  if (!lang)
    return; // no (syntax-define-language "ext" ...) yet — silently no-op,
            // matching map-key's own fallback when its prefix isn't
            // registered yet

  if (lang->num_words >= lang->words_cap) {
    new_cap = lang->words_cap ? lang->words_cap * 2 : 32;
    mem = grealloc(lang->words, (size_t)new_cap * sizeof(SyntaxWord));
    if (!mem)
      return;
    lang->words = (SyntaxWord *)mem;
    lang->words_cap = new_cap;
  }

  w = &lang->words[lang->num_words++];
  memset(w, 0, sizeof(*w));
  strncpy(w->word, word, sizeof(w->word) - 1);
  w->is_type = is_type;
}

/**
 * @brief Lisp function to add one keyword to a registered language
 * @details (syntax-add-keyword "ext" "word") — colors any identifier
 * matching `word` exactly (not a substring/prefix match) with the theme's
 * "keyword" color. Call syntax-define-language for `ext` first; called
 * before that, this silently does nothing.
 * @param args Lisp list: (extension word)
 * @return lisp_null()
 */
Lisp l_syntaxAddKeyword(Lisp args, LispError *e, LispContext ctx) {
  const char *ext = lisp_string(lisp_car(args));
  const char *word;
  args = lisp_cdr(args);
  word = lisp_string(lisp_car(args));
  syntaxAddWord(ext, word, 0);
  return lisp_null();
}

/**
 * @brief Lisp function to add one type name to a registered language
 * @details (syntax-add-type "ext" "word") — same as syntax-add-keyword,
 * but colors with the theme's "type" color instead.
 * @param args Lisp list: (extension word)
 * @return lisp_null()
 */
Lisp l_syntaxAddType(Lisp args, LispError *e, LispContext ctx) {
  const char *ext = lisp_string(lisp_car(args));
  const char *word;
  args = lisp_cdr(args);
  word = lisp_string(lisp_car(args));
  syntaxAddWord(ext, word, 1);
  return lisp_null();
}

/**
 * @brief Lisp function: color a word inside comments
 * @details (syntax-add-comment-word "ext" "word" "role" ["next-role"]) —
 * inside a comment of an `ext` file, `word` is drawn in `role`'s theme
 * color ("keyword", "type", "string", "number", ...) instead of the comment
 * color, and with `next-role`, the word after it too: the doxygen package
 * registers ("@param" "keyword" "type") so the parameter's name stands
 * out. The word must stand alone ("@param" in "@params" doesn't count).
 * Works whether or not syntax-define-language has run for `ext` yet.
 * @return #t, or #f for an unknown role or a word longer than 31 bytes
 */
Lisp l_syntaxAddCommentWord(Lisp args, LispError *e, LispContext ctx) {
  const char *ext;
  const char *word;
  const char *role_name;
  int role;
  int next_role = -1;
  (void)ctx;
  if (!lisp_is_pair(args) || !lisp_is_pair(lisp_cdr(args)) || !lisp_is_pair(lisp_cdr(lisp_cdr(args))) ||
      lisp_type(lisp_car(args)) != LISP_STRING || lisp_type(lisp_car(lisp_cdr(args))) != LISP_STRING ||
      lisp_type(lisp_car(lisp_cdr(lisp_cdr(args)))) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  ext = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  word = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  role_name = lisp_string(lisp_car(args));
  role = syntaxRoleFromName(role_name);
  args = lisp_cdr(args);
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING) {
    next_role = syntaxRoleFromName(lisp_string(lisp_car(args)));
    if (next_role < 0) {
      editorSetStatusMessage("syntax-add-comment-word: unknown role %s", lisp_string(lisp_car(args)));
      return lisp_make_bool(0);
    }
  }
  if (role < 0) {
    editorSetStatusMessage("syntax-add-comment-word: unknown role %s", role_name);
    return lisp_make_bool(0);
  }
  return lisp_make_bool(syntaxAddCommentWord(ext, word, role, next_role));
}

/**
 * @brief Lisp function: (write-file "path" "text") — writes text to path
 * as is (binary: no newline translation on Windows), replacing it.
 * @return #t, or #f if the file couldn't be written
 */
Lisp l_writeFile(Lisp args, LispError *e, LispContext ctx) {
  const char *path = lisp_string(lisp_car(args));
  const char *text = lisp_string(lisp_car(lisp_cdr(args)));
  size_t len = strlen(text);
  FILE *f;
  int ok;
  (void)e;
  (void)ctx;
  f = fopen(path, "wb");
  if (!f)
    return lisp_false();
  ok = fwrite(text, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  return lisp_make_bool(ok);
}

/**
 * @brief Lisp function: color whole lines by how they start
 * @details (syntax-add-line-prefix "ext" "prefix" "role") — in that
 * language, a row starting with "prefix" is drawn entirely in the theme's
 * "role" color: "keyword", "type", "string", "comment", "number",
 * "default", or "added"/"removed" (meant for diffs). Rules are tried in
 * the order they were added, so add longer prefixes ("+++") before shorter
 * ones ("+"). The language must already exist (syntax-define-language).
 * @return #t, or #f if the language is unknown, the role is unknown or the
 * language already has 12 rules
 */
Lisp l_syntaxAddLinePrefix(Lisp args, LispError *e, LispContext ctx) {
  const char *ext = lisp_string(lisp_car(args));
  const char *prefix;
  const char *role_name;
  LanguageSyntax *lang;
  int role;
  (void)e;
  (void)ctx;
  args = lisp_cdr(args);
  prefix = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  role_name = lisp_string(lisp_car(args));

  lang = findLanguageByExtension(ext);
  role = syntaxRoleFromName(role_name);
  if (!lang || role < 0 ||
      lang->num_line_prefixes >= (int)(sizeof(lang->line_prefixes) / sizeof(lang->line_prefixes[0])))
    return lisp_false();
  snprintf(lang->line_prefixes[lang->num_line_prefixes].prefix,
           sizeof(lang->line_prefixes[0].prefix), "%s", prefix);
  lang->line_prefixes[lang->num_line_prefixes].role = role;
  lang->num_line_prefixes++;
  return lisp_true();
}

/**
 * @brief Lisp function: color text between two markers on a row
 * @details (syntax-add-span "ext" "open" "close" "role") — in that
 * language, text from "open" to the next "close" on the same row is drawn
 * in "role", which besides the color roles of syntax-add-line-prefix can
 * be "bold" or "italic"; the markers themselves get the comment color.
 * Markdown-style flanking applies (no word character just outside a
 * marker, no blank just inside), so "snake_case" or "a * b" don't match.
 * Spans are tried in the order they were added: add "**" before "*". The
 * language must already exist (syntax-define-language).
 * @return #t, or #f if the language is unknown, the role is unknown, a
 * marker is empty or too long (7 bytes) or the language already has 8
 * spans
 */
Lisp l_syntaxAddSpan(Lisp args, LispError *e, LispContext ctx) {
  const char *ext = lisp_string(lisp_car(args));
  const char *open;
  const char *close;
  LanguageSyntax *lang;
  int role;
  (void)e;
  (void)ctx;
  args = lisp_cdr(args);
  open = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  close = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  role = syntaxRoleFromName(lisp_string(lisp_car(args)));

  lang = findLanguageByExtension(ext);
  if (!lang || role < 0 || !*open || !*close ||
      strlen(open) >= sizeof(lang->spans[0].open) || strlen(close) >= sizeof(lang->spans[0].close) ||
      lang->num_spans >= (int)(sizeof(lang->spans) / sizeof(lang->spans[0])))
    return lisp_false();
  strcpy(lang->spans[lang->num_spans].open, open);
  strcpy(lang->spans[lang->num_spans].close, close);
  lang->spans[lang->num_spans].role = role;
  lang->num_spans++;
  return lisp_true();
}

/**
 * @brief Lisp function: highlight a buffer as a given language
 * @details (buffer-set-syntax buffer-id "ext") — the buffer is highlighted
 * with the rules registered for "ext" whatever its name; "" goes back to
 * choosing by file name. For generated buffers like a diff view.
 * @return #t, or #f if there's no such buffer
 */
Lisp l_bufferSetSyntax(Lisp args, LispError *e, LispContext ctx) {
  struct buffer_t *buf;
  (void)e;
  (void)ctx;
  buf = bufferFindById((int)lisp_int(lisp_car(args)));
  if (!buf)
    return lisp_false();
  snprintf(buf->syntax, sizeof(buf->syntax), "%s", lisp_string(lisp_car(lisp_cdr(args))));
  syntaxHighlighterInvalidateAfter(buf, 0);
  return lisp_true();
}

Lisp editorGetPaneWitdh(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  return lisp_make_int(active->width);
}

Lisp editorGetPaneHeight(Lisp args, LispError *e, LispContext ctx) {
  EditorPane *active = splitScreenGetActivePane();
  return lisp_make_int(active->height);
}

/**
 * @brief Lisp function to start recording a keyboard macro.
 * @return lisp_null()
 * @see macroStartRecording()
 */
Lisp macroStart(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  macroStartRecording();
  return lisp_null();
}

/**
 * @brief Lisp function to stop recording the keyboard macro.
 * @return lisp_null()
 * @see macroStopRecording()
 */
Lisp macroEnd(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  macroStopRecording();
  return lisp_null();
}

/**
 * @brief Lisp function to replay the last keyboard macro, (macro-run) once
 * or (macro-run n) n times.
 * @return lisp_null()
 * @see macroRun()
 */
Lisp macroRunBuiltin(Lisp args, LispError *e, LispContext ctx) {
  int times;
  (void)e;
  (void)ctx;
  times = 1;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT)
    times = (int)lisp_int(lisp_car(args));
  macroRun(times);
  return lisp_null();
}

/**
 * @brief Lisp function to repaint the whole screen on the next frame, for
 * when something else drew over it.
 * @return lisp_null()
 * @see editorFreeScreenCache()
 */
Lisp editorRedraw(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  (void)ctx;
  // Dropping both caches makes the next frame go out in full.
  editorFreeScreenCache();
  return lisp_null();
}
