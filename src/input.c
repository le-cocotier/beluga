#include "../include/input.h"
#include "../include/macro.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/completion.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/output.h"
#include "../include/split_screen.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/buffer_switcher.h"
#include "../include/file_finder.h"
#include "../include/list_popup.h"
#include "../include/lsp_ui.h"
#include "../include/pty_session.h"
#include "../include/syntax_highlighter.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "include/cJSON.h"

/**
 * @brief The id of the "*" (global) prefix, looked up by name and cached.
 * @details A keybind registered under "*" matches regardless of
 * E.prefix_state (see executeKeyBind()) — it's how e.g. a single CTRL-g
 * binding resets to no-prefix from inside any other prefix. Looked up by
 * name rather than hardcoded: init.c registers "*" as a built-in prefix,
 * but doesn't guarantee it a particular id (unlike "no-prefix", which
 * several other files do hardcode as id 0), so this has to ask
 * find_prefix() instead of assuming an index.
 *
 * find_prefix() itself is unconditionally registered by initEditor()
 * before any config loads, so the "name not found" fallback it has (it
 * returns E.prefix[0], i.e. "no-prefix") is unreachable here in practice
 * — noted because that fallback would otherwise be a real problem: it'd
 * make this return "no-prefix"'s own id, and every "no-prefix" keybind
 * would then match unconditionally, the same as an actual "*" one does.
 */
static int starPrefixId(void) {
  static int id = -2; // -2: not yet looked up: distinct from -1 (find_prefix's
                      // own not-found)
  if (id == -2)
    id = find_prefix("*").prefix_id;
  return id;
}

/**
 * @file input.c
 * @brief Input handling module for the Beluga text editor
 * @details Manages user input processing, key bindings, cursor movement, and
 * file path completion
 */

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
 * @brief Returns the first file completion match for the given path
 * @details Searches the directory containing the given path prefix and returns
 * the first file or directory entry that matches the filename prefix.
 * Appends a trailing slash for directory entries.
 * @param path The file path to complete (can be relative or absolute)
 * @return Pointer to the completed file path (dynamically allocated), or NULL
 * if:
 *         - path ends with '/' (already a directory)
 *         - no matching entries found
 *         - directory cannot be opened
 * @note Caller is responsible for bFreeing the returned string
 * @note Uses static buffer internally; may return stale pointers across calls
 */
Utf8Char *fileCompletion(Utf8Char *path) {
  OsDirEntry *entries;
  int count = 0;
  int i;
  Utf8Char directory[PATH_MAX] = {0};
  Utf8Char predict[PATH_MAX] = {0};
  const Utf8Char *last_slash = lastSlashUtf8(path);
  Utf8Char *full_path = gcalloc(PATH_MAX, sizeof(Utf8Char));
  Utf8Char *entry_name_utf8 = NULL;
  int predict_len = 0;
  char *dir_path = NULL;

  if (*(last_slash + 1)->character == '\0') {
    // path is a directory
    utf8Printf(directory, PATH_MAX, "%u", path);
    END_OF_STRING(directory[utf8StringLen(directory) - 2]);
  } else {
    utf8NCpy(directory, path, (int)(last_slash - path));
    predict_len = utf8Printf(predict, PATH_MAX, "%u", last_slash + 1);
  }

  dir_path = (char *)utf8ToString(directory);
  entries = os_list_dir(dir_path, &count);
  gfree(dir_path);
  if (!entries) {
    return full_path;
  }

  for (i = 0; i < count; i++) {
    entry_name_utf8 = stringToUtf8((unsigned char *)entries[i].name);
    if ((predict_len == 0) ||
        (utf8StringNCmp(entry_name_utf8, predict, predict_len) == 0)) {
      utf8Printf(full_path, PATH_MAX, "%u/%u", directory, entry_name_utf8);
      gfree(entry_name_utf8);
      if (entries[i].is_dir) {
        full_path[utf8StringLen(full_path) - 1].character[0] = '/';
        full_path[utf8StringLen(full_path) - 1].length = 1;
      }
      os_free_dir(entries, count);
      return full_path;
    }
    gfree(entry_name_utf8);
  }

  os_free_dir(entries, count);
  appDebug("[FILE COMP] no entries\n");
  utf8Cpy(full_path, path);
  return full_path;
}

/**
 * @brief Displays an interactive prompt and returns user input
 * @details Allows the user to enter text in a prompt with optional path
 * completion via Tab key. Supports backspace, delete, and escape key handling.
 * Dynamically allocates memory for the input buffer.
 * @param prompt The prompt message format string (printf-style)
 * @param placeHolder Initial text to display in the input buffer
 * @param bPathMode If non-zero, enables Tab key file path completion
 * @return Pointer to the user-entered text (dynamically allocated), or NULL if:
 *         - User pressed ESC to cancel
 *         - Input buffer is empty when Enter is pressed
 * @note Caller is responsible for bFreeing the returned string
 * @note Uses editorReadKey() for input and editorRefreshScreen() for display
 */
Utf8Char *editorPrompt(const char *prompt, Utf8Char *placeHolder,
                       char bPathMode) {
  return editorPromptEx(prompt, placeHolder, bPathMode, 0);
}

/**
 * @brief editorPrompt(), optionally accepting an empty answer
 * @param allowEmpty If non-zero, ENTER on an empty input returns an empty
 * string (only ESC returns NULL) — for answers where "nothing" is valid,
 * like the replacement text of a query-replace.
 */
Utf8Char *editorPromptEx(const char *prompt, Utf8Char *placeHolder,
                         char bPathMode, int allowEmpty) {
  Utf8Char *buf = gcalloc(PATH_MAX, sizeof(Utf8Char));
  int buf_len = 0;
  Utf8Char *c = NULL;
  Utf8Char *full_path = NULL;
  Utf8Char path[PATH_MAX];
  char *pwd;
  char *buf_str;
  int absolute;
  if (placeHolder != NULL) {
    buf_len = utf8Printf(buf, PATH_MAX, "%u",
                         placeHolder); // zero not counted by snprintf
  }
  // Publish the in-progress input so the which-key panel can filter its
  // candidates on it live. Borrowed, not owned — cleared on every exit path
  // below, before `buf` is returned to the caller or freed.
  E.prompt_input = buf;
  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    c = editorReadKey();
    if (*c->character == DEL_KEY || *c->character == BACKSPACE) {
      if (buf_len != 0) {
        --buf_len;
        END_OF_STRING(buf[buf_len]);
      }
    } else if (*c->character == ESCAPE) {
      editorSetStatusMessage("Aborted");
      gfree(c);
      E.prompt_input = NULL;
      return NULL;
    } else if (*c->character == '\r') {
      if (buf_len != 0 || allowEmpty) {
        editorSetStatusMessage("");
        gfree(c);
        E.prompt_input = NULL;
        return buf;
      }
    } else if (bPathMode && *c->character == '\t') {
      // Completion of first path
      // getenv is allocating memory
      buf_str = (char *)utf8ToString(buf);
      absolute = os_path_is_absolute(buf_str);
      gfree(buf_str);
      if (!absolute) {
        pwd = os_getcwd(); // not $PWD: Windows shells don't set it
        appDebug("%s\n", pwd ? pwd : "");
        // Replace path by pwd + last word
        utf8Printf(path, PATH_MAX, "%s/%u", pwd ? pwd : ".", buf);
        free(pwd);
      } else {
        // already absolute path
        utf8Printf(path, PATH_MAX, "%u", buf);
      }
      // TODO(TODO.md): path completion — readdir order, leaks, single match
      full_path = fileCompletion(path);
      if (full_path) {
        buf_len = utf8Printf(buf, PATH_MAX, "%u", full_path);
        gfree(full_path);
      }
    } else if (*c->character) {
      (buf + buf_len)->length = c->length;
      memcpy((buf + buf_len)->character, c->character, c->length);
      buf_len++;
    }
    gfree(c);
  }
}

/**
 * @brief Executes the command bound to a key sequence
 * @details Searches the keybinding table for a matching key sequence and
 * prefix state, then evaluates the associated Lisp command.
 * @param key_sequence The string representation of the key sequence
 * @return 1 if a matching keybinding was found and executed, 0 otherwise
 * @note Updates global editor state E (prefix_state)
 * @note Uses Lisp interpreter to evaluate bound commands
 */
int executeKeyBind(char *key_sequence) {
  int i;
  int previous_state = 0;
  for (i = 0; i < E.number_of_keybinds; ++i) {
    if (!strcmp(key_sequence, E.key_binds[i].key_sequence)) {
      if ((E.key_binds[i].prefix_id != starPrefixId()) &&
          (E.prefix_state != E.key_binds[i].prefix_id)) {
        continue;
      }
      previous_state = E.prefix_state;
      // It's a symbol, create a function call
      lisp_eval(lisp_cons(E.key_binds[i].command, lisp_null(), E.ctx),
                &E.ctx_error, E.ctx);
      // Sticky prefixes (see add-prefix) skip the auto-reset — they're
      // meant to stay active across keystrokes that don't themselves
      // change the prefix (e.g. arrow-key navigation in a sidebar),
      // unlike a one-shot "user"-style CTRL-x <key> prefix.
      if (E.prefix_state == previous_state &&
          !E.prefix[E.key_binds[i].prefix_id].sticky)
        E.prefix_state = 0;
      return 1;
    }
  }
  return 0;
}

static void ptyForwardKey(struct buffer_t *buf, Utf8Char *key) {
  unsigned char seq[8];
  int len;

  switch (key->character[0]) {
  case ARROW_UP:
    memcpy(seq, "\x1b[A", 3);
    len = 3;
    break;
  case ARROW_DOWN:
    memcpy(seq, "\x1b[B", 3);
    len = 3;
    break;
  case ARROW_RIGHT:
    memcpy(seq, "\x1b[C", 3);
    len = 3;
    break;
  case ARROW_LEFT:
    memcpy(seq, "\x1b[D", 3);
    len = 3;
    break;
  case DEL_KEY:
    memcpy(seq, "\x1b[3~", 4);
    len = 4;
    break;
  case PAGE_UP:
    memcpy(seq, "\x1b[5~", 4);
    len = 4;
    break;
  case PAGE_DOWN:
    memcpy(seq, "\x1b[6~", 4);
    len = 4;
    break;
  case BEG_LINE:
    memcpy(seq, "\x1b[H", 3);
    len = 3;
    break;
  case END_LINE:
    memcpy(seq, "\x1b[F", 3);
    len = 3;
    break;
  default:
    memcpy(seq, key->character, key->length);
    len = (int)key->length;
    break;
  }

  ptySessionWrite(buf, (char *)seq, len);
}

/**
 * @brief Asks for signature help when `key` just opened a call's argument
 * list or started its next argument — i.e. it was "(" or "," and that's
 * what now sits right before the cursor, however it got inserted (plain
 * typing, or a binding such as smart_delimiters' "(" that also inserts
 * the ")"). Off with (define LSP-SIGNATURE-HELP #f).
 */
static void maybeRequestSignatureHelp(struct buffer_t *buf, const Utf8Char *key) {
  unsigned char ch;
  LspClient *lsp = lspClientForBuffer(buf);
  if (!E.constantes.LSP || !E.constantes.LSP_SIGNATURE_HELP || !lsp ||
      lsp->state != LSP_READY || !buf || !buf->b_lsp_open)
    return;
  if (key->length != 1 || (key->character[0] != '(' && key->character[0] != ','))
    return;
  if (buf->y >= buf->numrows || buf->x <= 0 || buf->x > buf->row[buf->y].size)
    return;
  ch = buf->row[buf->y].characters[buf->x - 1].character[0];
  if (ch != key->character[0])
    return;
  lspRequestSignatureHelp(lsp, buf, buf->y, buf->x);
}

/**
 * @brief Processes a single keypress from the user
 * @details Reads a key, checks if it matches any registered keybinding,
 * and either executes the bound command or inserts the character. Resets
 * the quit buffer counter on successful key processing.
 * @note Updates global editor state E
 * @note Calls editorReadKey() to get input and editorInsertChar() for unbound
 * keys
 */
void editorProcessKeypress() {
  Utf8Char *c;
  EditorPane *active;
  struct buffer_t *buf;
  CompletionItem *item;
  Row *row;
  int i;
  int first_buffer_id;
  struct buffer_t *b;
  int is_printable;
  int is_word;
  int should_trigger_completion;
  const char *trig;
  int trig_len;
  int match;
  int k;
  LspClient *lsp;

  macroNoteKeySequenceStart();
  c = editorReadKey();

  // Modal: while the buffer switcher is open, it owns every key —
  // checked before anything else so it can't be shadowed by a
  // keybind or by LSP completion.
  if (bufferSwitcherHandleKey(c)) {
    gfree(c);
    return;
  }

  // Same reasoning as the buffer switcher above — the generic list
  // popup is modal too, so it gets first refusal on every key.
  if (listPopupHandleKey(c)) {
    gfree(c);
    return;
  }

  // ...and the fuzzy file finder, likewise modal — it needs every
  // printable key to build its query, so it can't sit behind keybind
  // dispatch.
  if (fileFinderHandleKey(c)) {
    gfree(c);
    return;
  }

  active = splitScreenGetActivePane();
  buf = bufferFindById(active->buffer_id);

  if (E.constantes.LSP && buf->b_lsp_open) {

    if (lspClientForBuffer(buf) && E.lsp_completion.visible) {
      if (*c->character == '\r') {
        item = &E.lsp_completion.items[E.lsp_completion.selected];
        lspAcceptCompletion(buf, item);
        E.lsp_completion.visible = 0;
        gfree(c);
        return;
      }

      if (lspUiHandleKey(&E.lsp_completion, c)) {
        gfree(c);
        return;
      }
    }
  }

  // Hover / signature popup: ESC closes it (and goes no further); any other
  // key also closes a hover, then carries on as usual.
  if (lspUiInfoHandleKey(&E.lsp_info, c)) {
    gfree(c);
    return;
  }

  E.dirty = 0;
  if (E.buffers != NULL) {
    first_buffer_id = E.buffers->buffer_id;
    b = E.buffers;
    do {
      E.dirty += b->dirty;
      b = b->next;
    } while (b->buffer_id != first_buffer_id);
  }

  if (executeKeyBind(keyToString(c))) {
    // "(" is usually bound (smart_delimiters inserts the pair), so it
    // never reaches the plain-insert path below.
    maybeRequestSignatureHelp(buf, c);
    gfree(c);
    return;
  }

  if (buf->type == TERMINAL_BUFF) {
    ptyForwardKey(buf, c);
    gfree(c);
    return;
  }

  // A prefix is pending (e.g. CTRL-x's "user") and this key isn't bound
  // under it — it's not a valid continuation of whatever command was
  // being chorded, so it must not fall through to normal editing below.
  // It used to: every key unbound under the active prefix landed here and
  // got inserted as a literal character (silently, since bufferInsertChar
  // only refuses TERMINAL_BUFF — a real, writable FILE_BUFF took it
  // without complaint), while prefix_state stayed exactly as pending as
  // before. A user who fat-fingers or hesitates mid-chord — or just
  // returns after stepping away with CTRL-x half-pressed — would see every
  // subsequent keystroke seemingly do nothing (CTRL-x CTRL-c doesn't fire
  // "CTRL-x" isn't bound under "user") while actually typing garbage into
  // the open file one character at a time, looking exactly like a freeze
  // with no visible cause. A one-shot prefix (the default — see
  // add-prefix's doc comment) is only ever good for a single follow-up
  // key, matched or not, so consume it here the same way executeKeyBind()
  // already does on a successful match; a sticky prefix (search, explorer)
  // stays active, same as it already does through a match.
  if (E.prefix_state != 0) {
    if (!E.prefix[E.prefix_state].sticky)
      E.prefix_state = 0;
    gfree(c);
    return;
  }

  is_printable = utf8IsPrintable(c);
  is_word = is_word_char(c);

  if (is_printable) {
    bufferInsertChar(c);
    E.quit_times_buffer = E.constantes.QUIT_TIMES;
  }

  should_trigger_completion =
      E.constantes.LSP && buf->b_lsp_open && is_printable && is_word;

  // Word-char keystrokes aren't the only thing worth asking the
  // server about — member-access completion (by far the most common
  // trigger in most languages: "obj." in Python, "->"/"::" in C++)
  // fires on punctuation instead. Check the server's own advertised
  // trigger strings against the text ending at the cursor now that
  // this keystroke has actually been inserted.
  lsp = lspClientForBuffer(buf);
  if (!should_trigger_completion && E.constantes.LSP && buf->b_lsp_open &&
      is_printable && lsp && buf->numrows > 0 &&
      buf->y < buf->numrows) {
    row = &buf->row[buf->y];
    for (i = 0;
         i < lsp->num_trigger_chars && !should_trigger_completion;
         i++) {
      trig = lsp->trigger_chars[i];
      trig_len = (int)strlen(trig);
      if (trig_len == 0 || trig_len > buf->x)
        continue;

      match = 1;
      for (k = 0; k < trig_len; k++) {
        if (row->characters[buf->x - trig_len + k].character[0] !=
            (unsigned char)trig[k]) {
          match = 0;
          break;
        }
      }
      if (match)
        should_trigger_completion = 1;
    }
  }

  maybeRequestSignatureHelp(buf, c);

  gfree(c);
  c = NULL;

  // Debounced: this only (re)starts an idle timer. The actual
  // didChange+completion round-trip fires from lspCompletionDebounceTick()
  // once typing pauses, instead of on every single keystroke.
  if (E.constantes.LSP && lsp) {
    if (should_trigger_completion && lsp->state == LSP_READY)
      lspScheduleCompletion();
    else
      lspCancelScheduledCompletion();
  }
}
