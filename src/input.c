#include "../include/input.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/completion.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/output.h"
#include "../include/split_screen.h"
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

static int compareNames(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

void fileCompletionFree(FileCompletion *fc) {
  int i;
  for (i = 0; i < fc->count; i++)
    gfree(fc->names[i]);
  gfree(fc->names);
  gfree(fc->dir);
  memset(fc, 0, sizeof(*fc));
}

/**
 * @brief Collects the entries of `path`'s directory that start with its
 * last component
 * @details `path` is split at its last '/': "/a/b/fo" lists "/a/b/" for
 * names starting with "fo"; "/a/b/" lists everything in "/a/b/". Names are
 * sorted, and directories get a trailing '/'. `common` is the length of
 * the prefix all matches share, cut back to a whole UTF-8 character.
 * @param path UTF-8 path to complete (absolute, or relative to the cwd)
 * @param fc filled in; release with fileCompletionFree()
 * @return number of matches (0 if the directory can't be read)
 */
int fileCompletionMatches(const char *path, FileCompletion *fc) {
  OsDirEntry *entries;
  const char *slash = strrchr(path, '/');
  const char *prefix = slash ? slash + 1 : path;
  size_t dir_len = slash ? (size_t)(slash - path) + 1 : 0;
  size_t prefix_len = strlen(prefix);
  size_t name_len;
  size_t n;
  int count = 0;
  int i;
  char *list_dir;

  memset(fc, 0, sizeof(*fc));
  fc->dir = galloc(dir_len + 1);
  if (!fc->dir)
    return 0;
  memcpy(fc->dir, path, dir_len);
  fc->dir[dir_len] = '\0';

  list_dir = dir_len ? fc->dir : ".";
  entries = os_list_dir(list_dir, &count);
  if (!entries)
    return 0;
  fc->names = galloc((size_t)(count ? count : 1) * sizeof(char *));
  if (!fc->names) {
    os_free_dir(entries, count);
    return 0;
  }
  for (i = 0; i < count; i++) {
    if (strncmp(entries[i].name, prefix, prefix_len) != 0)
      continue;
    // Hidden entries only when asked for with a leading '.'
    if (entries[i].name[0] == '.' && prefix[0] != '.')
      continue;
    name_len = strlen(entries[i].name);
    fc->names[fc->count] = galloc(name_len + 2);
    if (!fc->names[fc->count])
      continue;
    memcpy(fc->names[fc->count], entries[i].name, name_len);
    if (entries[i].is_dir)
      fc->names[fc->count][name_len++] = '/';
    fc->names[fc->count][name_len] = '\0';
    fc->count++;
  }
  os_free_dir(entries, count);

  if (fc->count == 0)
    return 0;
  qsort(fc->names, (size_t)fc->count, sizeof(char *), compareNames);

  fc->common = strlen(fc->names[0]);
  for (i = 1; i < fc->count; i++) {
    for (n = 0; n < fc->common && fc->names[i][n] == fc->names[0][n]; n++)
      ;
    fc->common = n;
  }
  // Never stop in the middle of a multi-byte character.
  while (fc->common > 0 &&
         ((unsigned char)fc->names[0][fc->common] & 0xC0) == 0x80)
    fc->common--;
  return fc->count;
}

/**
 * @brief Writes dir + the first `len` bytes of `name` into the prompt
 * buffer
 * @return the new length of the buffer, in characters
 */
static int setPromptPath(Utf8Char *buf, const char *dir, const char *name,
                         size_t len) {
  char *joined = galloc(strlen(dir) + len + 1);
  Utf8Char *as_utf8;
  int buf_len;
  if (!joined)
    return utf8StringLen(buf) - 1;
  strcpy(joined, dir);
  strncat(joined, name, len);
  as_utf8 = stringToUtf8((unsigned char *)joined);
  gfree(joined);
  buf_len = utf8Printf(buf, PATH_MAX, "%u", as_utf8);
  gfree(as_utf8);
  return buf_len;
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
  Utf8Char *buf = gcalloc(PATH_MAX, sizeof(Utf8Char));
  int buf_len = 0;
  Utf8Char *c = NULL;
  Utf8Char path[PATH_MAX];
  char *pwd;
  char *buf_str;
  char *path_str;
  int absolute;
  // TAB completion: the matches of the last TAB, and which one a repeated
  // TAB is showing (-1: none yet, the buffer holds their common prefix).
  FileCompletion fc = {0};
  int cycle = -1;
  int last_was_tab = 0;
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
      fileCompletionFree(&fc);
      E.prompt_input = NULL;
      return NULL;
    } else if (*c->character == '\r') {
      if (buf_len != 0) {
        editorSetStatusMessage("");
        gfree(c);
        fileCompletionFree(&fc);
        E.prompt_input = NULL;
        return buf;
      }
    } else if (bPathMode && *c->character == '\t' && last_was_tab &&
               fc.count > 1) {
      // Repeated TAB on an ambiguous prefix: step through the matches.
      cycle = (cycle + 1) % fc.count;
      buf_len = setPromptPath(buf, fc.dir, fc.names[cycle],
                              strlen(fc.names[cycle]));
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
      // Complete as far as all matches agree; a single match is completed
      // whole. Another TAB then cycles through the matches.
      fileCompletionFree(&fc);
      cycle = -1;
      path_str = (char *)utf8ToString(path);
      if (fileCompletionMatches(path_str, &fc) > 0)
        buf_len = setPromptPath(buf, fc.dir, fc.names[0], fc.common);
      else
        buf_len = utf8Printf(buf, PATH_MAX, "%u", path);
      gfree(path_str);
    } else if (*c->character) {
      (buf + buf_len)->length = c->length;
      memcpy((buf + buf_len)->character, c->character, c->length);
      buf_len++;
    }
    last_was_tab = bPathMode && *c->character == '\t';
    gfree(c);
  }
}

/**
 * @brief Does this binding apply in `buf`?
 * @return 0 if it doesn't, 1 for a global binding, 2 for one scoped to the
 * buffer's file extension (which takes precedence over a global one)
 */
int keyBindScope(const struct keyBind_t *kb, const struct buffer_t *buf) {
  char ext[32];
  char needle[34];
  if (!kb->extensions)
    return 1;
  if (!buf || !utf8FilenameExtension(buf->filename, ext, sizeof(ext)))
    return 0;
  snprintf(needle, sizeof(needle), " %s ", ext);
  return strstr(kb->extensions, needle) ? 2 : 0;
}

/**
 * @brief Executes the command bound to a key sequence
 * @details Searches the keybinding table for a matching key sequence and
 * prefix state, then evaluates the associated Lisp command. A binding
 * scoped to the active buffer's extension (map-key's 4th argument) wins
 * over a global one.
 * @param key_sequence The string representation of the key sequence
 * @return 1 if a matching keybinding was found and executed, 0 otherwise
 * @note Updates global editor state E (prefix_state)
 * @note Uses Lisp interpreter to evaluate bound commands
 */
int executeKeyBind(char *key_sequence) {
  int i;
  int previous_state = 0;
  int found = -1;
  int found_scope = 0;
  int scope;
  const struct buffer_t *buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
  for (i = 0; i < E.number_of_keybinds; ++i) {
    if (strcmp(key_sequence, E.key_binds[i].key_sequence) != 0)
      continue;
    if ((E.key_binds[i].prefix_id != starPrefixId()) &&
        (E.prefix_state != E.key_binds[i].prefix_id))
      continue;
    scope = keyBindScope(&E.key_binds[i], buf);
    if (scope > found_scope) {
      found = i;
      found_scope = scope;
    }
  }
  if (found < 0)
    return 0;

  previous_state = E.prefix_state;
  // It's a symbol, create a function call
  lisp_eval(lisp_cons(E.key_binds[found].command, lisp_null(), E.ctx),
            &E.ctx_error, E.ctx);
  // Sticky prefixes (see add-prefix) skip the auto-reset — they're
  // meant to stay active across keystrokes that don't themselves
  // change the prefix (e.g. arrow-key navigation in a sidebar),
  // unlike a one-shot "user"-style CTRL-x <key> prefix.
  if (E.prefix_state == previous_state &&
      !E.prefix[E.key_binds[found].prefix_id].sticky)
    E.prefix_state = 0;
  return 1;
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
  if (!E.constantes.LSP || !E.constantes.LSP_SIGNATURE_HELP || !E.lsp_client ||
      E.lsp_client->state != LSP_READY || !buf || !buf->b_lsp_open)
    return;
  if (key->length != 1 || (key->character[0] != '(' && key->character[0] != ','))
    return;
  if (buf->y >= buf->numrows || buf->x <= 0 || buf->x > buf->row[buf->y].size)
    return;
  ch = buf->row[buf->y].characters[buf->x - 1].character[0];
  if (ch != key->character[0])
    return;
  lspRequestSignatureHelp(E.lsp_client, buf, buf->y, buf->x);
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
  Utf8Char *c = editorReadKey();
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

    if (E.lsp_client && E.lsp_completion.visible) {
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
  if (!should_trigger_completion && E.constantes.LSP && buf->b_lsp_open &&
      is_printable && E.lsp_client && buf->numrows > 0 &&
      buf->y < buf->numrows) {
    row = &buf->row[buf->y];
    for (i = 0;
         i < E.lsp_client->num_trigger_chars && !should_trigger_completion;
         i++) {
      trig = E.lsp_client->trigger_chars[i];
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
  if (E.constantes.LSP && E.lsp_client) {
    if (should_trigger_completion && E.lsp_client->state == LSP_READY)
      lspScheduleCompletion();
    else
      lspCancelScheduledCompletion();
  }
}
