#include "../include/input.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/output.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/completion.h"
#include "../include/data.h"
#include "../include/split_screen.h"
#include <ctype.h>
#include <dirent.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/lsp_ui.h"
#include "../include/buffer_switcher.h"
#include "../include/list_popup.h"
#include "../include/syntax_highlighter.h"
#include "../include/pty_session.h"
#include "include/cJSON.h"

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
Utf8Char *fileCompletion(Utf8Char *path)
{
  DIR *dir;
  struct dirent *entry;
  Utf8Char directory[PATH_MAX] = {0};
  Utf8Char file[PATH_MAX] = {0};
  Utf8Char predict[PATH_MAX] = {0};
  Utf8Char *last_slash = lastSlashUtf8(path);
  Utf8Char *full_path = calloc(PATH_MAX, sizeof(Utf8Char));
  Utf8Char *entry_name_utf8 = NULL;
  int predict_len = 0;
  size_t dir_len;
  char *dir_path = NULL;
  char *entry_name = NULL;
  char *full_path_str = NULL;
  struct stat st;

  if (*(last_slash + 1)->character == '\0')
  {
    // path is a directory
    utf8Printf(directory, "%u", path);
    END_OF_STRING(directory[utf8StringLen(directory) - 2])
  }
  else
  {
    utf8NCpy(directory, path, (last_slash - path));
    predict_len = utf8Printf(predict, "%u", last_slash + 1);
  }

  dir_path = utf8ToString(directory);
  dir = opendir(dir_path);
  free(dir_path);
  if (!dir)
    return path;

  // skip . and ..
  readdir(dir);
  readdir(dir);

  while ((entry = readdir(dir)) != NULL)
  {
    entry_name = entry->d_name;
    entry_name_utf8 = stringToUtf8(entry_name);
    if ((predict_len == 0) || (utf8StringNCmp(entry_name_utf8, predict, predict_len) == 0))
    {
      utf8Printf(full_path, "%u/%u", directory, entry_name_utf8);
      free(entry_name_utf8);


      full_path_str = utf8ToString(full_path);
      if (stat(full_path_str, &st) == 0 && S_ISDIR(st.st_mode))
      {
        full_path[utf8StringLen(full_path) - 1].character[0] = '/';
        full_path[utf8StringLen(full_path) - 1].length = 1;
      }
      free(full_path_str);
      closedir(dir);

      return full_path;
    }
    free(entry_name_utf8);
  }

  // Cleanup when no more entries
  closedir(dir);
  dir = NULL;
  free(entry);
  free(full_path);
  appDebug("[FILE COMP] no entries\n");
  return path;
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
Utf8Char *editorPrompt(char *prompt, Utf8Char *placeHolder, char bPathMode)
{
  Utf8Char *buf = calloc(PATH_MAX, sizeof(Utf8Char));
  int buf_len = 0;
  Utf8Char *c = NULL;
  Utf8Char *full_path = NULL;
  if (placeHolder != NULL)
  {
    buf_len = utf8Printf(buf, "%u", placeHolder); // zero not counted by snprintf
  }
  while (1)
  {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    c = editorReadKey();
    if (*c->character == DEL_KEY || *c->character == BACKSPACE)
    {
      if (buf_len != 0)
      {
        --buf_len;
        END_OF_STRING(buf[buf_len]);
      }
    }
    else if (*c->character == ESCAPE)
    {
      editorSetStatusMessage("Aborted");
      free(c);
      return NULL;
    }
    else if (*c->character == '\r')
    {
      if (buf_len != 0)
      {
        editorSetStatusMessage("");
        free(c);
        return buf;
      }
    }
    else if (bPathMode && *c->character == '\t')
    {
      // Completion of first path
      Utf8Char path[PATH_MAX];
      char *pwd; // getenv is allocating memory
      if (*buf[0].character != '/')
      {
        pwd = getenv("PWD");
        appDebug("%s\n", pwd);
        // Replace path by pwd + last word
        utf8Printf(path, "%s/%u", pwd, buf);
      }
      else
      {
        // already absolute path
        utf8Printf(path, "%u", buf);
      }
      // TODO: upgrade this
      full_path = fileCompletion(path);
      if (full_path)
      {
        buf_len = utf8Printf(buf, "%u", full_path);
      }
    }
    else if (*c->character)
    {
      (buf + buf_len)->length = c->length;
      memcpy((buf + buf_len)->character, c->character, c->length);
      buf_len++;
    }
    free(c);
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
int executeKeyBind(char *key_sequence)
{
  int i;
  int previous_state = 0;
  for (i = 0; i < E.number_of_keybinds; ++i)
  {
    if (!strcmp(key_sequence, E.key_binds[i].key_sequence))
    {
      if (E.prefix_state != E.key_binds[i].prefix_id)
      {
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
      if (E.prefix_state == previous_state && !E.prefix[E.key_binds[i].prefix_id].sticky)
        E.prefix_state = 0;
      return 1;
    }
  }
  return 0;
}

void ptyForwardKey(struct buffer_t *buf, Utf8Char *key)
{
  unsigned char seq[8];
  int len;

  switch (key->character[0])
  {
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
    len = key->length;
    break;
  }

  ptySessionWrite(buf, (char *)seq, len);
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
void editorProcessKeypress()
{
    Utf8Char *c = editorReadKey();

        // Modal: while the buffer switcher is open, it owns every key —
        // checked before anything else so it can't be shadowed by a
        // keybind or by LSP completion.
        if (bufferSwitcherHandleKey(c)) {
            free(c);
            return;
        }

        // Same reasoning as the buffer switcher above — the generic list
        // popup is modal too, so it gets first refusal on every key.
        if (listPopupHandleKey(c)) {
            free(c);
            return;
        }

        EditorPane *active = splitScreenGetActivePane();
        struct buffer_t *buf = bufferFindById(active->buffer_id);

        if (E.constantes.LSP && buf->b_lsp_open)
        {

            if (E.lsp_client && E.lsp_completion.visible) {
                if (*c->character == '\r') {
                    CompletionItem *item = &E.lsp_completion.items[E.lsp_completion.selected];

                    int file_col = active->cursor_x + active->x_offset;
                    Row *row = &buf->row[active->cursor_y + active->y_offset];

                    // is_word_char() treats '#' as part of a "word" so the syntax
                    // highlighter can match whole keywords like "#include" — but
                    // clangd's completion items for preprocessor directives never
                    // include the '#' in insertText (the '#' is already typed and
                    // out of band from its point of view). Walking through it here
                    // would over-count already_typed by one and silently drop the
                    // first character of every such completion, so stop the scan
                    // right after any '#' instead of consuming it.
                    int word_start = file_col;
                    while (word_start > 0 && is_word_char(&row->characters[word_start - 1])
                           && row->characters[word_start - 1].character[0] != '#')
                        word_start--;

                    int already_typed = file_col - word_start;
                    const char *suffix = item->insertText + already_typed;
                    for (int i = 0; suffix[i]; i++)
                    {
                        Utf8Char ch = { .character = { suffix[i], 0, 0, 0 }, .length = 1 };
                        bufferInsertChar(&ch);
                    }

                    E.lsp_completion.visible = 0;
                    free(c);
                    return;
                }

                if (lspUiHandleKey(&E.lsp_completion, c)) {
                    free(c);
                    return;
                }
            }
        }

        E.dirty = 0;
        if (E.buffers != NULL)
        {
            int first_buffer_id = E.buffers->buffer_id;
            struct buffer_t* b = E.buffers;
            do
            {
                E.dirty += b->dirty;
                b = b->next;
            } while (b->buffer_id != first_buffer_id);
        }

        if (executeKeyBind(keyToString(c)))
        {
            free(c);
            return;
        }

        if (buf->type == TERMINAL_BUFF)
        {
            ptyForwardKey(buf, c);
            free(c);
            return;
        }

        int is_printable = utf8IsPrintable(c);
        int is_word = is_word_char(c);

        if (is_printable)
        {
            bufferInsertChar(c);
            E.quit_times_buffer = E.constantes.QUIT_TIMES;
        }

        int should_trigger_completion = E.constantes.LSP && buf->b_lsp_open &&
                                         is_printable && is_word;

        // Word-char keystrokes aren't the only thing worth asking the
        // server about — member-access completion (by far the most common
        // trigger in most languages: "obj." in Python, "->"/"::" in C++)
        // fires on punctuation instead. Check the server's own advertised
        // trigger strings against the text ending at the cursor now that
        // this keystroke has actually been inserted.
        if (!should_trigger_completion && E.constantes.LSP && buf->b_lsp_open &&
            is_printable && E.lsp_client && buf->numrows > 0 && buf->y < buf->numrows)
        {
            Row *row = &buf->row[buf->y];
            for (int i = 0; i < E.lsp_client->num_trigger_chars && !should_trigger_completion; i++)
            {
                const char *trig = E.lsp_client->trigger_chars[i];
                int trig_len = (int) strlen(trig);
                if (trig_len == 0 || trig_len > buf->x) continue;

                int match = 1;
                for (int k = 0; k < trig_len; k++)
                {
                    if (row->characters[buf->x - trig_len + k].character[0] != (unsigned char) trig[k])
                    { match = 0; break; }
                }
                if (match) should_trigger_completion = 1;
            }
        }

        free(c);
        c = NULL;

        // Debounced: this only (re)starts an idle timer. The actual
        // didChange+completion round-trip fires from lspCompletionDebounceTick()
        // once typing pauses, instead of on every single keystroke.
        if (E.constantes.LSP && E.lsp_client)
        {
            if (should_trigger_completion && E.lsp_client->state == LSP_READY)
                lspScheduleCompletion();
            else
                lspCancelScheduledCompletion();
        }
}
