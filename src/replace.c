/**
 * @file replace.c
 * @brief Query-replace, in one buffer or across the project
 * @details Each match is shown selected and answered with one key:
 *   y or SPACE  replace it          n or DEL  skip it
 *   !           replace the rest of this file
 *   A           replace every remaining match, in every file
 *   q, ESC or ENTER  stop
 * Matching is literal and case-sensitive, within a line, like the search
 * commands. All the replacements made in one buffer undo as one step.
 * Nothing is saved: touched buffers are left modified, as LSP rename does.
 */

#include "../include/replace.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/input.h"
#include "../include/output.h"
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utf8.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

typedef enum {
  REPLACE_ASK,      // ask at every match
  REPLACE_ALL_FILE, // '!': the rest of the current buffer, then ask again
  REPLACE_ALL,      // 'A': everything left
  REPLACE_QUIT,
} ReplaceMode;

/**
 * @brief Number of characters (not bytes) in a UTF-8 string.
 */
static int charCount(const char *s) {
  int n = 0;
  for (; *s; s++)
    if (((unsigned char)*s & 0xC0) != 0x80)
      n++;
  return n;
}

/**
 * @brief Character index in `row` of byte offset `byte`.
 */
static int byteToChar(const Row *row, int byte) {
  int bytes = 0;
  int i;
  for (i = 0; i < row->size; i++) {
    if (bytes >= byte)
      return i;
    bytes += row->characters[i].length;
  }
  return row->size;
}

/**
 * @brief Byte offset in `row` of character index `col`.
 */
static int charToByte(const Row *row, int col) {
  int bytes = 0;
  int i;
  for (i = 0; i < col && i < row->size; i++)
    bytes += row->characters[i].length;
  return bytes;
}

/**
 * @brief Copies `s` for use inside an editorSetStatusMessage() format.
 */
static void formatSafe(const char *s, char *out, size_t out_size) {
  size_t i;
  snprintf(out, out_size, "%s", s);
  for (i = 0; out[i]; i++)
    if (out[i] == '%')
      out[i] = '?';
}

/**
 * @brief Shows the match at (row, col, len) selected and reads one key.
 */
static unsigned char askAtMatch(struct buffer_t *buf, int row, int col, int len,
                       const char *from, const char *to, int project) {
  char from_safe[128];
  char to_safe[128];
  Utf8Char *key;
  unsigned char answer;

  // The match drawn as the region: mark at its end, cursor at its start.
  buf->y = row;
  buf->x = col;
  buf->mark_active = 1;
  buf->mark_y = row;
  buf->mark_x = col + len;

  formatSafe(from, from_safe, sizeof(from_safe));
  formatSafe(to, to_safe, sizeof(to_safe));
  editorSetStatusMessage("Replace \"%s\" with \"%s\"? y/n, ! rest of file%s, q quit",
                         from_safe, to_safe, project ? ", A all files" : "");
  editorRefreshScreen();
  // editorReadKey() gives a 0 character when no key came in time.
  do {
    key = editorReadKey();
    answer = key ? key->character[0] : 'q';
    gfree(key);
  } while (answer == 0);
  buf->mark_active = 0;
  return answer;
}

/**
 * @brief Runs the replace over `buf` from (start_row, start_col) to its end.
 * @param mode In: how to proceed; out: how the next buffer should (QUIT
 * when the user stopped).
 * @return The number of replacements made.
 */
static int replaceInBuffer(struct buffer_t *buf, int start_row, int start_col,
                           const char *from, const char *to, ReplaceMode *mode,
                           int project) {
  const int from_len = charCount(from);
  const int to_len = charCount(to);
  int count = 0;
  int row = start_row;
  int col = start_col;
  int byte;
  int match_col;
  char *row_str;
  char *match;
  unsigned char answer;
  int replace;
  int grouped = 0;

  if (buf->state == READ_ONLY || buf->type != FILE_BUFF || from_len == 0)
    return 0;
  if (*mode == REPLACE_ALL_FILE)
    *mode = REPLACE_ASK; // '!' was for the previous file

  while (row < buf->numrows && *mode != REPLACE_QUIT) {
    row_str = (char *)utf8ToString(buf->row[row].characters);
    if (!row_str)
      break;
    byte = charToByte(&buf->row[row], col);
    match = strstr(row_str + byte, from);
    if (!match) {
      gfree(row_str);
      row++;
      col = 0;
      continue;
    }
    match_col = byteToChar(&buf->row[row], (int)(match - row_str));
    gfree(row_str);

    replace = 1;
    if (*mode == REPLACE_ASK) {
      answer = askAtMatch(buf, row, match_col, from_len, from, to, project);
      switch (answer) {
      case 'y':
      case 'Y':
      case ' ':
        break;
      case 'n':
      case 'N':
      case DEL_KEY:
      case BACKSPACE:
        replace = 0;
        break;
      case '!':
        *mode = REPLACE_ALL_FILE;
        break;
      case 'A':
        if (project) {
          *mode = REPLACE_ALL;
          break;
        }
        replace = 0; // not offered in a single buffer: same as n
        break;
      default: // q, ESC, ENTER, anything else
        *mode = REPLACE_QUIT;
        replace = 0;
        break;
      }
    }

    if (replace) {
      if (!grouped) {
        bufferUndoGroupBegin(buf);
        grouped = 1;
      }
      bufferApplyTextEdit(buf, row, match_col, row, match_col + from_len, to);
      count++;
      col = match_col + to_len; // never re-match inside the replacement
    } else {
      col = match_col + 1;
    }
  }

  if (grouped)
    bufferUndoGroupEnd(buf);
  return count;
}

/**
 * @brief Prompts for the text to find and its replacement.
 * @return 1 with both set (galloc'd C strings), 0 if cancelled.
 */
static int promptFromTo(const char *what, char **from, char **to) {
  char prompt[256];
  char from_safe[128];
  Utf8Char *in;

  snprintf(prompt, sizeof(prompt), "%s: %%u (ESC to cancel)", what);
  in = editorPrompt(prompt, NULL, 0);
  if (!in)
    return 0;
  *from = (char *)utf8ToString(in);
  gfree(in);
  if (!*from || !**from) {
    gfree(*from);
    return 0;
  }

  formatSafe(*from, from_safe, sizeof(from_safe));
  snprintf(prompt, sizeof(prompt), "%s \"%s\" with: %%u (ESC to cancel)", what,
           from_safe);
  // Empty is a valid replacement (deleting every match).
  in = editorPromptEx(prompt, NULL, 0, 1);
  if (!in) {
    gfree(*from);
    return 0;
  }
  *to = (char *)utf8ToString(in);
  gfree(in);
  if (!*to) {
    gfree(*from);
    return 0;
  }
  return 1;
}

void queryReplacePrompt(void) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  char *from;
  char *to;
  ReplaceMode mode = REPLACE_ASK;
  int count;
  int y;
  int x;

  if (!buf || buf->type != FILE_BUFF || buf->state == READ_ONLY) {
    editorSetStatusMessage("Nothing to replace in this buffer");
    return;
  }
  // Run from CTRL-x r: with the prefix still pending, the which-key panel
  // would cover the matches.
  E.prefix_state = 0;
  if (!promptFromTo("Query replace", &from, &to))
    return;

  y = buf->y;
  x = buf->x;
  count = replaceInBuffer(buf, buf->y, buf->x, from, to, &mode, 0);
  // Stopped: stay on the match. Ran to the end: back where it started.
  if (mode != REPLACE_QUIT) {
    buf->y = y < buf->numrows ? y : 0;
    buf->x = buf->numrows ? (x <= buf->row[buf->y].size ? x : 0) : 0;
  }
  editorSetStatusMessage("Replaced %d occurrence%s", count,
                         count == 1 ? "" : "s");
  gfree(from);
  gfree(to);
}

void projectQueryReplacePrompt(void) {
  char *from;
  char *to;
  char **files = NULL;
  int n_files;
  int i;
  int id;
  int count = 0;
  int n;
  int touched = 0;
  ReplaceMode mode = REPLACE_ASK;
  struct buffer_t *buf;
  EditorPane *active;

  E.prefix_state = 0;
  if (!promptFromTo("Replace in project", &from, &to))
    return;

  n_files = projectSearchFiles(from, NULL, &files);
  if (n_files <= 0) {
    editorSetStatusMessage("No matches in the project");
    gfree(from);
    gfree(to);
    return;
  }

  for (i = 0; i < n_files && mode != REPLACE_QUIT; i++) {
    id = projectSearchOpenFile(files[i]);
    if (id == -1)
      continue;
    active = splitScreenGetActivePane();
    active->buffer_id = id;
    buf = bufferFindById(id);
    if (!buf)
      continue;
    n = replaceInBuffer(buf, 0, 0, from, to, &mode, 1);
    count += n;
    if (n > 0)
      touched++;
  }

  editorSetStatusMessage("Replaced %d occurrence%s in %d file%s (not saved)",
                         count, count == 1 ? "" : "s", touched,
                         touched == 1 ? "" : "s");
  for (i = 0; i < n_files; i++)
    gfree(files[i]);
  gfree(files);
  gfree(from);
  gfree(to);
}
