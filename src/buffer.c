/**
 * @file buffer.c
 * @brief Buffer management implementation for multiple open files
 */

#include "../include/buffer.h"
#include "../include/breakpoints.h"
#include "../include/ansi_parser.h"
#include "../include/autosave.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include "../include/galloc.h"
#include "../include/split_screen.h"
#include "../include/utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/builtins.h"
#include "../include/completion.h"
#include "../include/input.h"
#include "../include/pty_session.h"
#include "../include/subprocess.h"
#include "../include/syntax_highlighter.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "include/os.h"

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;
extern int buffer_id;

/***********************************************************************************************************************
 *
 *                                                 undo / redo
 *
 **********************************************************************************************************************/

#define UNDO_MAX_ENTRIES                                                       \
  4000 // per buffer, per stack — oldest entries evicted past this

/**
 * @brief Appends one entry to an UndoList, growing it geometrically and
 * evicting the oldest entry once UNDO_MAX_ENTRIES is reached so a very long
 * editing session can't grow this without bound.
 */
static void textInsertRaw(struct buffer_t *buf, int row, int col,
                          const char *text, int *end_row, int *end_col);
static char *textDeleteRaw(struct buffer_t *buf, int start_row, int start_col,
                           int end_row, int end_col);

/**
 * @brief Frees what an UndoList's entries own (the TEXT kinds' strings)
 * and empties it.
 */
static void undoListClear(UndoList *list) {
  int i;
  for (i = 0; i < list->count; i++)
    gfree(list->entries[i].text);
  list->count = 0;
}

void bufferClearUndo(struct buffer_t *buf) {
  undoListClear(&buf->undo_stack);
  undoListClear(&buf->redo_stack);
}

static void undoListPush(UndoList *list, const UndoEntry entry) {
  int new_cap;
  UndoEntry *grown;
  if (list->count >= UNDO_MAX_ENTRIES) {
    gfree(list->entries[0].text);
    memmove(list->entries, list->entries + 1,
            sizeof(UndoEntry) * (list->count - 1));
    list->count--;
  }
  if (list->count >= list->capacity) {
    new_cap = list->capacity ? list->capacity * 2 : 64;
    grown = grealloc(list->entries, sizeof(UndoEntry) * new_cap);
    if (!grown) {
      gfree(entry.text);
      return; // best-effort: drop the entry rather than crash
    }
    list->entries = grown;
    list->capacity = new_cap;
  }
  list->entries[list->count++] = entry;
}

/**
 * @brief Records one atomic edit for undo, called from the row-mutation
 * call sites (bufferInsertChar, bufferDelChar, bufferInsertNewLine, ...) —
 * never from bufferRowInsertChar/bufferRowDelChar/rowSplitAt/rowJoinNext
 * themselves, since bufferUndo()/bufferRedo() replay edits by calling those
 * same low-level primitives directly and must not re-record what they're
 * already moving between the undo and redo stacks.
 * @details Any new edit invalidates the redo stack (standard undo/redo
 * behavior — redoing after a fresh edit would replay a now-nonsensical
 * future). Consecutive entries of the same kind at contiguous positions
 * (typing or backspacing a run of characters, or forward-deleting a run at
 * a fixed column) are folded into one group so a single undo/redo call
 * reverses/replays the whole run at once; anything else — a different kind,
 * a cursor jump, or a fresh call right after an undo/redo — starts a new
 * group.
 */
static void undoRecord(struct buffer_t *buf, const UndoKind kind, const int row,
                       const int col, const Utf8Char ch) {
  int contiguous;
  UndoEntry entry;
  undoListClear(&buf->redo_stack);

  contiguous = buf->undo_group_hold;
  if (!contiguous && buf->undo_group_open && buf->undo_last_kind == kind &&
      buf->undo_last_row == row &&
      // typing forward, or backspacing (col-1)/forward-deleting (col) at a
      // fixed column
      ((kind == UNDO_INSERT_CHAR && col == buf->undo_last_col + 1) ||
       (kind == UNDO_DELETE_CHAR &&
        (col == buf->undo_last_col - 1 || col == buf->undo_last_col))))
    contiguous = 1;
  if (!contiguous)
    buf->undo_next_group_id++;

  entry = (UndoEntry){.kind = kind,
                      .row = row,
                      .col = col,
                      .ch = ch,
                      .group_id = buf->undo_next_group_id};
  undoListPush(&buf->undo_stack, entry);

  buf->undo_group_open = 1;
  buf->undo_last_kind = kind;
  buf->undo_last_row = row;
  buf->undo_last_col = col;
}

void bufferUndoGroupBegin(struct buffer_t *buf) {
  buf->undo_next_group_id++;
  buf->undo_group_hold = 1;
}

void bufferUndoGroupEnd(struct buffer_t *buf) {
  buf->undo_group_hold = 0;
  // The next edit starts a group of its own.
  buf->undo_group_open = 0;
}

/**
 * @brief Records a bulk edit (UNDO_INSERT_TEXT or UNDO_DELETE_TEXT) as one
 * entry, taking ownership of `text`.
 * @param join_previous Add it to the group of the entry recorded just
 * before (a replace is a delete plus an insert, undone together).
 */
static void undoRecordText(struct buffer_t *buf, const UndoKind kind,
                           const int row, const int col, char *text,
                           const int join_previous) {
  UndoEntry entry;
  undoListClear(&buf->redo_stack);
  // Inside bufferUndoGroupBegin()/End() everything shares one group.
  if (!buf->undo_group_hold && (!join_previous || buf->undo_stack.count == 0))
    buf->undo_next_group_id++;
  entry = (UndoEntry){.kind = kind,
                      .row = row,
                      .col = col,
                      .text = text,
                      .group_id = buf->undo_next_group_id};
  undoListPush(&buf->undo_stack, entry);
  // Typing right after a paste starts its own group.
  buf->undo_group_open = 0;
  buf->undo_last_kind = kind;
}

/**
 * @brief Where inserting `text` at (row, col) ends.
 */
static void textEndPosition(const char *text, int row, int col, int *end_row,
                            int *end_col) {
  const unsigned char *p;
  for (p = (const unsigned char *)text; *p; p++) {
    if (*p == '\n') {
      row++;
      col = 0;
    } else if ((*p & 0xC0) != 0x80) {
      col++;
    }
  }
  *end_row = row;
  *end_col = col;
}

/**
 * @brief 1 when the characters of `r` before `col` are only spaces and tabs
 * (none at all included).
 */
static int rowHeadIsBlank(const Row *r, int col) {
  int i;
  for (i = 0; i < col && i < r->size; i++)
    if (r->characters[i].length != 1 ||
        (r->characters[i].character[0] != ' ' &&
         r->characters[i].character[0] != '\t'))
      return 0;
  return 1;
}

/**
 * @brief Splits `row` into two rows at character index `col` — the tail
 * ([col, size)) becomes a new row right after it. Pure row mutation: no
 * cursor movement, no undo recording (callers do both themselves).
 */
static void rowSplitAt(struct buffer_t *buf, int row, int col) {
  Row *r = &buf->row[row];
  const int head_blank = rowHeadIsBlank(r, col);
  bufferInsertRow(buf, row + 1, &r->characters[col], r->size - col);
  r = &buf->row[row]; // bufferInsertRow's recalloc may have moved the array
  r->size = col;
  END_OF_STRING(r->characters[r->size]);
  // ENTER before a line's code: its breakpoint goes down with the code.
  if (head_blank)
    breakpointsLineMoved(buf, row, row + 1);
}

/**
 * @brief Merges row `row + 1` into `row` (appending its text) and removes
 * row `row + 1`. Inverse of rowSplitAt() at the same (row, col) where col
 * is `row`'s size just before the join. Pure row mutation, same caveats
 * as rowSplitAt().
 */
static void rowJoinNext(struct buffer_t *buf, int row) {
  Row *next = &buf->row[row + 1];
  Row *dest = &buf->row[row];
  int i;
  for (i = 0; i < next->size; ++i)
    bufferRowInsertChar(buf, dest, dest->size, &next->characters[i]);
  gfree(next->characters);
  next->characters = NULL;
  memmove(&buf->row[row + 1], &buf->row[row + 2],
          sizeof(Row) * (buf->numrows - row - 2));
  buf->numrows--;
  breakpointsLinesRemoved(buf, row + 1, 1);
  splitScreenLinesShifted(buf, row + 1, -1);
  syntaxHighlighterInvalidateAfter(buf, row);
}

/**
 * @brief Inserts one blank row at `at`. Pure row mutation.
 */
static void rowInsertBlank(struct buffer_t *buf, int at) {
  Utf8Char end_string;
  END_OF_STRING(end_string);
  bufferInsertRow(buf, at, &end_string, 0);
}

/**
 * @brief Removes the row at `at` entirely. Inverse of rowInsertBlank().
 * Pure row mutation.
 */
static void rowRemoveAt(struct buffer_t *buf, int at) {
  bufferFreeRow(&buf->row[at]);
  memmove(&buf->row[at], &buf->row[at + 1],
          sizeof(Row) * (buf->numrows - at - 1));
  buf->numrows--;
  breakpointsLinesRemoved(buf, at, 1);
  splitScreenLinesShifted(buf, at, -1);
  syntaxHighlighterInvalidateAfter(buf, at - 1);
}

/**
 * @brief Undoes the most recent group of edits on `buf` (one atomic edit,
 * or a coalesced run of contiguous ones — see undoRecord()), moving each
 * entry onto the redo stack unchanged so bufferRedo() can replay it
 * forward again. Leaves the cursor at the last-undone edit's position.
 */
void bufferUndo(struct buffer_t *buf) {
  int gid;
  UndoEntry e;
  int end_row;
  int end_col;
  if (buf->undo_stack.count == 0) {
    editorSetStatusMessage("No further undo information");
    return;
  }

  gid = buf->undo_stack.entries[buf->undo_stack.count - 1].group_id;
  while (buf->undo_stack.count > 0 &&
         buf->undo_stack.entries[buf->undo_stack.count - 1].group_id == gid) {
    e = buf->undo_stack.entries[--buf->undo_stack.count];
    switch (e.kind) {
    case UNDO_INSERT_CHAR:
      bufferRowDelChar(buf, &buf->row[e.row], e.col);
      break;
    case UNDO_DELETE_CHAR:
      bufferRowInsertChar(buf, &buf->row[e.row], e.col, &e.ch);
      break;
    case UNDO_SPLIT_LINE:
      rowJoinNext(buf, e.row);
      break;
    case UNDO_JOIN_LINE:
      rowSplitAt(buf, e.row, e.col);
      break;
    case UNDO_INSERT_ROW:
      rowRemoveAt(buf, e.row);
      break;
    case UNDO_INSERT_TEXT:
      textEndPosition(e.text, e.row, e.col, &end_row, &end_col);
      gfree(textDeleteRaw(buf, e.row, e.col, end_row, end_col));
      break;
    case UNDO_DELETE_TEXT:
      textInsertRaw(buf, e.row, e.col, e.text, &end_row, &end_col);
      break;
    }
    undoListPush(&buf->redo_stack, e);
    buf->x = e.col;
    buf->y = e.row;
  }
  buf->undo_group_open =
      0; // the next fresh edit must not merge into what's left
  if (buf->type != TERMINAL_BUFF) {
    buf->dirty++;
    buf->edit_seq++;
  }
  editorSetStatusMessage("Undo");
}

/**
 * @brief Redoes the most recently undone group of edits on `buf`, moving
 * each entry back onto the undo stack unchanged. Leaves the cursor at the
 * last-redone edit's position.
 */
void bufferRedo(struct buffer_t *buf) {
  int gid;
  UndoEntry e;
  int end_row;
  int end_col;
  if (buf->redo_stack.count == 0) {
    editorSetStatusMessage("No further redo information");
    return;
  }

  gid = buf->redo_stack.entries[buf->redo_stack.count - 1].group_id;
  while (buf->redo_stack.count > 0 &&
         buf->redo_stack.entries[buf->redo_stack.count - 1].group_id == gid) {
    e = buf->redo_stack.entries[--buf->redo_stack.count];
    switch (e.kind) {
    case UNDO_INSERT_CHAR:
      bufferRowInsertChar(buf, &buf->row[e.row], e.col, &e.ch);
      break;
    case UNDO_DELETE_CHAR:
      bufferRowDelChar(buf, &buf->row[e.row], e.col);
      break;
    case UNDO_SPLIT_LINE:
      rowSplitAt(buf, e.row, e.col);
      break;
    case UNDO_JOIN_LINE:
      rowJoinNext(buf, e.row);
      break;
    case UNDO_INSERT_ROW:
      rowInsertBlank(buf, e.row);
      break;
    case UNDO_INSERT_TEXT:
      textInsertRaw(buf, e.row, e.col, e.text, &end_row, &end_col);
      break;
    case UNDO_DELETE_TEXT:
      textEndPosition(e.text, e.row, e.col, &end_row, &end_col);
      gfree(textDeleteRaw(buf, e.row, e.col, end_row, end_col));
      break;
    }
    undoListPush(&buf->undo_stack, e);
    buf->x = e.col;
    buf->y = e.row;
    if (e.kind == UNDO_INSERT_TEXT) {
      buf->x = end_col;
      buf->y = end_row;
    }
  }
  buf->undo_group_open = 0;
  if (buf->type != TERMINAL_BUFF) {
    buf->dirty++;
    buf->edit_seq++;
  }
  editorSetStatusMessage("Redo");
}

/***********************************************************************************************************************
 *
 *                                                 selection / region
 *
 **********************************************************************************************************************/

/**
 * @brief Normalizes (buf->mark_x, buf->mark_y) and (buf->x, buf->y) into
 * document order, regardless of which one the user set first.
 */
void bufferRegionBounds(struct buffer_t *buf, int *start_row, int *start_col,
                        int *end_row, int *end_col) {
  int before = (buf->mark_y < buf->y) ||
               (buf->mark_y == buf->y && buf->mark_x <= buf->x);
  if (before) {
    *start_row = buf->mark_y;
    *start_col = buf->mark_x;
    *end_row = buf->y;
    *end_col = buf->x;
  } else {
    *start_row = buf->y;
    *start_col = buf->x;
    *end_row = buf->mark_y;
    *end_col = buf->mark_x;
  }
}

/**
 * @brief Extracts the text in [start_row:start_col, end_row:end_col) as a
 * plain '\n'-joined, NUL-terminated C string. Caller must gfree() it.
 */
char *bufferGetRegionText(struct buffer_t *buf, int start_row, int start_col,
                          int end_row, int end_col) {
  size_t cap = 256, len = 0;
  char *text = galloc(cap);
  int row;
  Row *r;
  int from;
  int to;
  int c;
  Utf8Char *ch;
  char *grown;
  if (!text)
    return NULL;

  for (row = start_row; row <= end_row && row < buf->numrows; row++) {
    r = &buf->row[row];
    from = (row == start_row) ? start_col : 0;
    to = (row == end_row) ? end_col : r->size;
    if (to > r->size)
      to = r->size;

    for (c = from; c < to; c++) {
      ch = &r->characters[c];
      if (len + (size_t)ch->length + 2 > cap) {
        cap *= 2;
        grown = grealloc(text, cap);
        if (!grown) {
          gfree(text);
          return NULL;
        }
        text = grown;
      }
      memcpy(text + len, ch->character, ch->length);
      len += ch->length;
    }
    if (row != end_row) {
      if (len + 2 > cap) {
        cap *= 2;
        grown = grealloc(text, cap);
        if (!grown) {
          gfree(text);
          return NULL;
        }
        text = grown;
      }
      text[len++] = '\n';
    }
  }
  text[len] = '\0';
  return text;
}

/**
 * @brief Deletes [start_row:start_col, end_row:end_col) from `buf` as one
 * undo entry (UNDO_DELETE_TEXT). Leaves the cursor at the deletion's start.
 */
static void deleteRegion(struct buffer_t *buf, int start_row, int start_col,
                         int end_row, int end_col, int join_previous) {
  char *deleted;
  if (buf->state == READ_ONLY)
    return;
  // See bufferInsertChar()'s comment — kill-region reaching a terminal
  // buffer's rows this way is the same class of bug.
  if (buf->type == TERMINAL_BUFF)
    return;
  if (start_row > end_row || (start_row == end_row && start_col >= end_col))
    return;
  if (start_row >= buf->numrows)
    return;

  deleted = textDeleteRaw(buf, start_row, start_col, end_row, end_col);
  if (deleted && deleted[0])
    undoRecordText(buf, UNDO_DELETE_TEXT, start_row, start_col, deleted,
                   join_previous);
  else
    gfree(deleted);
  buf->x = start_col;
  buf->y = start_row;
  if (buf->type != TERMINAL_BUFF) {
    buf->dirty++;
    buf->edit_seq++;
  }
}

void bufferDeleteRegion(struct buffer_t *buf, int start_row, int start_col,
                        int end_row, int end_col) {
  deleteRegion(buf, start_row, start_col, end_row, end_col, 0);
}

/**
 * @brief Inserts `text` (may contain '\n') at (row, col) as one undo entry
 * (UNDO_INSERT_TEXT) and leaves the cursor after it.
 */
static void insertText(struct buffer_t *buf, int row, int col, const char *text,
                       int join_previous) {
  int end_row;
  int end_col;
  char *copy;
  if (!text || !text[0])
    return;
  copy = utilStrdup(text);
  if (!copy)
    return;
  textInsertRaw(buf, row, col, text, &end_row, &end_col);
  undoRecordText(buf, UNDO_INSERT_TEXT, row, col, copy, join_previous);
  buf->x = end_col;
  buf->y = end_row;
  if (buf->type != TERMINAL_BUFF) {
    buf->dirty++;
    buf->edit_seq++;
  }
}

void bufferInsertText(struct buffer_t *buf, const char *text) {
  if (!buf || buf->state == READ_ONLY || buf->type == TERMINAL_BUFF)
    return;
  if (buf->numrows > 0 && buf->y < buf->numrows &&
      buf->x > buf->row[buf->y].size)
    buf->x = buf->row[buf->y].size;
  if (buf->y > buf->numrows)
    buf->y = buf->numrows;
  insertText(buf, buf->y, buf->x, text, 0);
}

/***********************************************************************************************************************
 *
 *                                                 generic text-edit application
 *
 **********************************************************************************************************************/

/**
 * @brief Replaces [start_row:start_col, end_row:end_col) in `buf` with
 * new_text (may contain '\n', may be empty for a pure deletion). Unlike
 * bufferInsertChar()/bufferInsertNewLine(), this takes `buf` explicitly and
 * never touches splitScreenGetActivePane() — the target may not even be
 * the currently-displayed buffer (an LSP rename can touch files the user
 * hasn't switched to). Goes through the same undo-recorded primitives as
 * everything else, so LSP edits are undoable too.
 */
void bufferApplyTextEdit(struct buffer_t *buf, int start_row, int start_col,
                         int end_row, int end_col, const char *new_text) {
  int replaced;
  if (buf->state == READ_ONLY)
    return;
  // No LSP server is ever registered for a terminal's own pseudo-
  // extension, so this shouldn't be reachable for one in practice — kept
  // anyway, same reasoning as bufferInsertChar()'s comment, since nothing
  // else stops a future caller from targeting one directly.
  if (buf->type == TERMINAL_BUFF)
    return;

  // Defensive clamp: a server's reported range should always be in
  // bounds, but a stale/misaligned response should never be allowed to
  // read past the row array.
  if (start_row < 0)
    start_row = 0;
  if (start_col < 0)
    start_col = 0;
  if (end_row < 0)
    end_row = 0;
  if (end_col < 0)
    end_col = 0;
  if (start_row >= buf->numrows)
    start_row = buf->numrows > 0 ? buf->numrows - 1 : 0;
  if (end_row >= buf->numrows)
    end_row = buf->numrows > 0 ? buf->numrows - 1 : 0;
  if (buf->numrows > 0) {
    if (start_col > buf->row[start_row].size)
      start_col = buf->row[start_row].size;
    if (end_col > buf->row[end_row].size)
      end_col = buf->row[end_row].size;
  }

  replaced =
      start_row < end_row || (start_row == end_row && start_col < end_col);
  if (replaced)
    deleteRegion(buf, start_row, start_col, end_row, end_col, 0);
  buf->x = start_col;
  buf->y = start_row;
  // The insert joins the delete's group, so the replace undoes in one go.
  insertText(buf, start_row, start_col, new_text, replaced);
}

/***********************************************************************************************************************
 *
 *                                                 git integration
 *
 **********************************************************************************************************************/

/**
 * @brief Runs `git -C dir <args...>` (NULL-terminated) and returns its
 * stdout (stderr dropped), or NULL.
 * @details A 1s deadline: this runs synchronously inside bufferCreate() on
 * every file open, so it must never be the reason a file takes a visible
 * moment to appear. No output cap: git's output here is a branch name or
 * one file's diff. Arguments as argv, so paths need no quoting.
 */
static char *gitRun(const char *dir, ...) {
  const long GIT_COMMAND_TIMEOUT_MS = 1000;
  char *argv[16];
  const char *arg;
  int n = 0;
  va_list ap;

  argv[n++] = "git";
  argv[n++] = "-C";
  argv[n++] = (char *)dir;
  va_start(ap, dir);
  while (n < 15 && (arg = va_arg(ap, const char *)) != NULL)
    argv[n++] = (char *)arg;
  va_end(ap);
  argv[n] = NULL;
  return runArgvCaptureQuiet(argv, GIT_COMMAND_TIMEOUT_MS, 0, NULL);
}

/**
 * @brief Parses a unified-diff hunk header ("@@ -old_start[,old_lines]
 * +new_start[,new_lines] @@"). old_lines/new_lines default to 1 when git
 * omits them (a single-line hunk).
 * @return 1 on success, 0 if `line` isn't a hunk header.
 */
static int gitParseHunkHeader(const char *line, int *old_start, int *old_lines,
                              int *new_start, int *new_lines) {
  const char *minus = strchr(line, '-');
  const char *plus = strchr(line, '+');
  if (!minus || !plus)
    return 0;
  *old_lines = 1;
  *new_lines = 1;
  if (sscanf(minus, "-%d,%d", old_start, old_lines) < 1)
    return 0;
  if (sscanf(plus, "+%d,%d", new_start, new_lines) < 1)
    return 0;
  return 1;
}

/**
 * @brief Refreshes buf->git_branch/git_dirty/git_lines[] by shelling out to
 * git. Deliberately NOT called on every frame — editorRefreshScreen() runs
 * every ~50ms tick, and re-running three git subprocesses that often would
 * be far too slow. Called instead on buffer-open and after-save, which is
 * when the answer can actually have changed.
 */
void bufferRefreshGitInfo(struct buffer_t *buf) {
  char *dir_str;
  char *file_str;
  char *branch;
  char *status;
  char *diff;
  char *line;
  int old_start;
  int old_lines;
  int new_start;
  int new_lines;
  GitLineStatus status2;
  int i;
  int idx;
  gfree(buf->git_lines);
  buf->git_lines = NULL;
  buf->git_lines_count = 0;
  buf->git_available = 0;
  buf->git_dirty = 0;
  buf->git_branch[0] = '\0';

  if (buf->type != FILE_BUFF || !buf->path || !buf->fullname)
    return;

  dir_str = (char *)utf8ToString(buf->path);
  file_str = (char *)utf8ToString(buf->fullname);

  branch = gitRun(dir_str, "rev-parse", "--abbrev-ref", "HEAD", (char *)NULL);
  if (!branch) {
    gfree(dir_str);
    gfree(file_str);
    return; // not a git repo (or git isn't installed) — leave everything c
            // eared
  }
  strncpy(buf->git_branch, branch, sizeof(buf->git_branch) - 1);
  buf->git_branch[sizeof(buf->git_branch) - 1] = '\0';
  buf->git_available = 1;
  gfree(branch);

  status =
      gitRun(dir_str, "status", "--porcelain", "--", file_str, (char *)NULL);
  buf->git_dirty = status != NULL;
  gfree(status);

  if (buf->numrows > 0) {
    buf->git_lines = gcalloc(buf->numrows, sizeof(GitLineStatus));
    buf->git_lines_count = buf->git_lines ? buf->numrows : 0;
  }

  diff = gitRun(dir_str, "diff", "--no-color", "-U0", "--", file_str,
                (char *)NULL);
  if (diff && buf->git_lines) {
    line = strtok(diff, "\n");
    while (line) {
      if (line[0] == '@' && line[1] == '@') {
        if (gitParseHunkHeader(line, &old_start, &old_lines, &new_start,
                               &new_lines)) {
          if (new_lines > 0) {
            status2 = (old_lines == 0) ? GIT_LINE_ADDED : GIT_LINE_MODIFIED;
            for (i = 0; i < new_lines; i++) {
              idx = new_start - 1 + i; // 0-based
              if (idx >= 0 && idx < buf->git_lines_count)
                buf->git_lines[idx] = status2;
            }
          } else if (new_start > 0) {
            // Pure deletion: no added/changed line of its own to mark —
            // flag the line right before it as the nearest visible anchor.
            idx = new_start - 1;
            if (idx >= 0 && idx < buf->git_lines_count)
              buf->git_lines[idx] = GIT_LINE_MODIFIED;
          }
        }
      }
      line = strtok(NULL, "\n");
    }
  }
  gfree(diff);
  gfree(dir_str);
  gfree(file_str);
}

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/

/**
 * @brief Finds a buffer by filename
 * @param filename The filename to search for
 * @return Buffer ID if found, -1 if not found
 */
int bufferFindByFilename(Utf8Char *filename) {
  int first_buffer_id;
  Buffer *buffer;
  if (E.buffers == NULL)
    return -1;
  first_buffer_id = E.buffers->buffer_id;
  buffer = E.buffers;
  do {
    if (buffer->filename != NULL &&
        utf8StringCmp(buffer->filename, filename) == 0) {
      return buffer->buffer_id;
    }
    buffer = buffer->next;
  } while (buffer->buffer_id != first_buffer_id);
  return -1;
}

/**
 * @brief Finds another FILE_BUFF buffer that's already LSP-synced
 * (b_lsp_open) for the same file, by full path
 * @details Now that the same file can be open in more than one buffer
 * (see bufferCreate()), this is what keeps LSP sync correct: a server
 * only expects one textDocument/didOpen per URI (and, symmetrically,
 * exactly one didClose once every buffer viewing it is gone) — it has
 * no notion of "beluga has this file open twice." Both
 * bufferMaybeSyncLsp() and lspShutdown()'s didClose loop use this to
 * only actually notify the server on the first open / last close of a
 * given URI, regardless of how many buffers are viewing it or what
 * order they're processed in.
 * @param buf The buffer to find a sibling for (excluded from the search)
 * @return buffer_id of the other buffer, or -1 if none
 */
int bufferFindOtherLspOpen(struct buffer_t *buf) {
  int first_buffer_id;
  struct buffer_t *b;
  if (E.buffers == NULL || !buf->fullname)
    return -1;
  first_buffer_id = E.buffers->buffer_id;
  b = E.buffers;
  do {
    if (b->buffer_id != buf->buffer_id && b->type == FILE_BUFF &&
        b->b_lsp_open && b->fullname &&
        utf8StringCmp(b->fullname, buf->fullname) == 0) {
      return b->buffer_id;
    }
    b = b->next;
  } while (b->buffer_id != first_buffer_id);
  return -1;
}

/**
 * @brief Finds a buffer by ID
 * @param buff_id The buffer ID to find
 * @return Pointer to buffer, NULL if not found
 */
struct buffer_t *bufferFindById(int buff_id) {
  int first_buffer_id;
  Buffer *buffer;
  if (E.buffers == NULL)
    return NULL;
  first_buffer_id = E.buffers->buffer_id;
  buffer = E.buffers;
  do {
    if (buffer->buffer_id == buff_id) {
      return buffer;
    }
    buffer = buffer->next;
  } while (buffer->buffer_id != first_buffer_id);
  return NULL;
}

/**
 * @brief Points a file buffer at `path`: sets its filename (the last part),
 * fullname (the resolved absolute path) and path (the directory).
 * @details "~/" is expanded to the home directory ("~" is only $HOME to a
 * shell; to realpath()/fopen() it's a plain directory name), separators
 * become '/', and a path that doesn't exist yet is kept as given. Used when
 * a file is opened and by Save-as.
 * @return 0, or -1 when out of memory (the buffer is left unchanged).
 */
int bufferSetPath(struct buffer_t *buf, const char *path) {
  char expanded[PATH_MAX + 1];
  char fullname[PATH_MAX + 1];
  char directory[PATH_MAX + 1];
  char *home;
  const char *name;
  char *real;
  Utf8Char *new_filename;
  Utf8Char *new_fullname;
  Utf8Char *new_path;

  home = (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) ? os_home_dir()
                                                                 : NULL;
  if (home)
    snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
  else
    snprintf(expanded, sizeof(expanded), "%s", path);
  gfree(home);
  os_path_to_slashes(expanded); // "src\\main.c" given the Windows way

  real = os_realpath(expanded);
  // File doesn't exist yet, or path invalid — fall back to the raw path
  snprintf(fullname, sizeof(fullname), "%s", real ? real : expanded);
  gfree(real);
  utilDirname(fullname, directory, sizeof(directory));
  name = strrchr(expanded, '/');
  name = name ? name + 1 : expanded;

  new_filename = stringToUtf8((unsigned char *)name);
  new_fullname = stringToUtf8((unsigned char *)fullname);
  new_path = stringToUtf8((unsigned char *)directory);
  if (!new_filename || !new_fullname || !new_path) {
    gfree(new_filename);
    gfree(new_fullname);
    gfree(new_path);
    return -1;
  }
  gfree(buf->filename);
  gfree(buf->fullname);
  gfree(buf->path);
  buf->filename = new_filename;
  buf->fullname = new_fullname;
  buf->path = new_path;
  return 0;
}

/**
 * @brief Creates a new buffer for a file or terminal
 * @details Allocates buffer slot, loads file content (file buffers only),
 * saves buffer metadata. If a file buffer for this path is already open,
 * switches to the existing buffer instead.
 * @param path Absolute/relative file path (FILE_BUFF) or plain display name
 * (TERMINAL_BUFF)
 * @param state Read-only / read-write state
 * @param type FILE_BUFF or TERMINAL_BUFF
 * @return Buffer ID on success, -1 on failure
 */
int bufferCreate(const Utf8Char *path, enum bufferStatus_e state,
                 enum buffer_type type) {
  Utf8Char *filename;
  char *string_filename;
  const Utf8Char *last_slash;
  const Utf8Char *name_start;
  unsigned char temp_filename[5] = "term";
  int existing_id;
  struct buffer_t *new_buf;
  char *path_str;
  char *fullname_str;
  Lisp hook_args;
  EditorPane *active;
  struct prefix_t prefix;
  struct buffer_t *old_next;
  appDebug("Creating new buffer");

  filename = NULL;
  string_filename = NULL;

  if (type == FILE_BUFF) {
    last_slash = lastSlashUtf8(path);
    name_start = last_slash ? last_slash + 1 : path;

    filename = (Utf8Char *)gcalloc(utf8StringLen(name_start), sizeof(Utf8Char));
    if (!filename)
      return -1;
    utf8Cpy(filename, name_start);
  } else if (type == TERMINAL_BUFF) { // path is already just a plain name, no
    // "term" (the shell of CTRL-x t), or the name given (a debugged
    // program's, ptySessionSetNextProgram()).
    filename = path ? (Utf8Char *)gcalloc(utf8StringLen(path), sizeof(Utf8Char))
                    : stringToUtf8(temp_filename);
    if (filename && path)
      utf8Cpy(filename, path);
    // splitting needed
    if (!filename)
      return -1;
  } else // EXPLORER_BUFF: `path` IS the buffer's display name directly
         // (e.g. "explorer", "*tree*") — not a real filesystem path, so
         // no slash-splitting either. Distinct names let multiple kinds
         // of scratch buffer (explorer sidebar, tree dump, ...) coexist
         // without bufferFindByFilename() treating them as the same one.
  {
    filename = (Utf8Char *)gcalloc(utf8StringLen(path), sizeof(Utf8Char));
    if (!filename)
      return -1;
    utf8Cpy(filename, path);
  }

  // FILE_BUFF deliberately always creates a fresh, independent buffer —
  // the same file can be open in several buffers at once (each with its
  // own cursor/scroll/dirty state, editorOpen() re-reading the file
  // into its own row array), rather than jumping back to whichever
  // buffer already has it open. TERMINAL_BUFF/EXPLORER_BUFF keep the
  // dedup: reusing the one "term" buffer or a singleton scratch buffer
  // like "*explorer*"/"*tree*" by name is the whole point there.
  if (type != FILE_BUFF) {
    existing_id = bufferFindByFilename(filename);
    if (existing_id != -1) {
      gfree(filename);
      // Always the buffer's id: this used to return bufferSwitch()'s own
      // 0-on-success instead, so reusing a scratch buffer by name
      // (editor-scratch-open) handed Lisp buffer 0 — whatever that was.
      // Only a terminal is switched to here; a scratch buffer is left for
      // the caller to place, same as when it's freshly created below.
      if (type == TERMINAL_BUFF && bufferSwitch(existing_id) != 0)
        return -1;
      return existing_id;
    }
  }

  if (E.number_of_buffer >= 64) {
    editorSetStatusMessage("Error: maximum buffers reached (64)");
    gfree(filename);
    return -1;
  }

  new_buf = gcalloc(1, sizeof(struct buffer_t));
  new_buf->buffer_id = buffer_id++;
  new_buf->type = type;
  new_buf->state = state;
  new_buf->x = 0;
  new_buf->y = 0;
  new_buf->dirty = 0;
  new_buf->b_lsp_open = 0;

  new_buf->filename = gcalloc(utf8StringLen(filename), sizeof(Utf8Char));
  if (!new_buf->filename) {
    gfree(filename);
    return -1;
  }
  utf8Cpy(new_buf->filename, filename);
  gfree(filename); // done with the local copy either way

  E.number_of_buffer++;

  if (type == FILE_BUFF) {
    path_str = (char *)utf8ToString(path);
    bufferSetPath(new_buf, path_str);
    gfree(path_str);

    // bufferMaybeStartLsp() checks itself whether a server is
    // registered for this extension — no need to gate the call here too.
    bufferMaybeStartLsp(new_buf);

    appDebug("opening file");
    editorOpen(new_buf);
    appDebug("buffer opened");
    // Before LSP sync, so the server gets the recovered text.
    autosaveOfferRecovery(new_buf);

    // Only meaningful once the server's already running for an earlier
    // file — needs the text just loaded above, so it can't run any
    // earlier than this.
    bufferMaybeSyncLsp(new_buf);

    // Callers traditionally do `active->buffer_id = bufferCreate(...)`
    // themselves, but only after this function returns — too late for
    // a "buffer-opened" hook. Anything the hook calls that resolves
    // "the current buffer" via the active pane (line-text, cursor-row,
    // editor-move-cursor-*, ...) needs this buffer to already be
    // active, or it silently acts on whatever was open before.
    splitScreenGetActivePane()->buffer_id = new_buf->buffer_id;
    bufferRefreshGitInfo(new_buf);

    fullname_str = (char *)utf8ToString(new_buf->fullname);
    hook_args = lisp_cons(
        lisp_make_string2(fullname_str, E.ctx),
        lisp_cons(lisp_make_int(new_buf->buffer_id), lisp_null(), E.ctx),
        E.ctx);
    runHooks("buffer-opened", hook_args);
    gfree(fullname_str);
  } else if (type == TERMINAL_BUFF) // no real filesystem path — don't touch r
                                    // alpath/dirname
  {
    new_buf->pty = NULL;
    new_buf->path = NULL;
    if (path) {
      new_buf->fullname = gcalloc(utf8StringLen(path), sizeof(Utf8Char));
      if (new_buf->fullname)
        utf8Cpy(new_buf->fullname, path);
    } else {
      new_buf->fullname = stringToUtf8((unsigned char *)"term");
    }

    active = splitScreenGetActivePane();
    if (ptySessionSpawn(new_buf, NULL, active->height, active->width) != 0) {
      editorSetStatusMessage("Error: failed to start shell");
      E.number_of_buffer--; // undo the early increment above — this buffer n
                            // ver came up
      return -1;
    }
    prefix = find_prefix("term");
    E.prefix_state = prefix.prefix_id;
  } else // EXPLORER_BUFF: no real filesystem path, no rows yet — Lisp
         // populates content via editor-scratch-set-lines. Doesn't touch
         // the active pane or prefix state itself; callers that need the
         // sidebar split or "explorer" keybind routing set those up
         // explicitly (see editor-explorer-sidebar-open), since a scratch
         // buffer can also just be opened plainly (e.g. a tree dump).
  {
    new_buf->pty = NULL;
    new_buf->path = NULL;
    new_buf->fullname = NULL;
    new_buf->row = NULL;
    new_buf->numrows = 0;
  }

  // Reuses the already-declared char* rather than overwriting the
  // Utf8Char* `filename` with a byte string: assigning one to the other
  // is a type error that gcc 14 (Debian trixie's default) rejects
  // outright, having promoted -Wincompatible-pointer-types to an error.
  string_filename = (char *)utf8ToString(new_buf->filename);
  editorSetStatusMessage("Opened: %s (buffer %d)", string_filename,
                         new_buf->buffer_id);
  gfree(string_filename);
  if (E.buffers != NULL) {
    // Splice new_buf in right after E.buffers. The node that used to
    // be there (old_next) needs its ->prev fixed up too, or walking
    // backward from it forever finds the node that's no longer
    // actually its predecessor — without this, the very first buffer
    // ever created ends up with a ->prev that permanently points to
    // itself instead of wrapping around to the last one.
    old_next = E.buffers->next;
    new_buf->prev = E.buffers;
    new_buf->next = old_next;
    E.buffers->next = new_buf;
    old_next->prev = new_buf;
    E.buffers = new_buf;
  } else {
    new_buf->next = new_buf;
    new_buf->prev = new_buf;
    E.buffers = new_buf;
  }
  return new_buf->buffer_id;
}

/**
 * @brief Switches to a specific buffer by ID
 * @details Saves current buffer state, loads target buffer content and state.
 * @param buff_id The buffer ID to switch to
 * @return 0 on success, -1 on failure
 */
int bufferSwitch(int buff_id) {
  struct buffer_t *target = NULL;
  const char *prefix_name;
  struct prefix_t prefix;
  target = bufferFindById(buff_id);
  if (target == NULL) {
    editorSetStatusMessage("Error: buffer not found");
    return -1;
  }
  E.layout.panes[E.layout.active_pane].buffer_id = buff_id;

  // Same focus-prefix routing splitScreenSwitchPane() does, so a buffer
  // that carries its own keymap gets it however you arrive: the buffer
  // switcher and CTRL-x ARROW-RIGHT come through here, not through a
  // pane switch. This also covers re-opening the terminal while its
  // buffer already exists — bufferCreate() dedups to bufferSwitch() and
  // returns before ever reaching the code that sets the "term" prefix.
  prefix_name = NULL;
  if (target->type == TERMINAL_BUFF)
    prefix_name = "term";
  else if (target->type == EXPLORER_BUFF)
    prefix_name = target->focus_prefix[0] ? target->focus_prefix : "explorer";

  if (prefix_name) {
    prefix = find_prefix(prefix_name);
    if (prefix.prefix_id != -1)
      E.prefix_state = prefix.prefix_id;
  } else {
    E.prefix_state = 0; // plain file buffer: never inherit a stale keymap
  }

  editorSetStatusMessage("Switched to: %u (buffer %d)", target->filename,
                         buff_id);
  return 0;
}

/**
 * @brief Asks before killing something that can't be recovered: a file
 * buffer with unsaved changes, or a terminal whose shell is still running.
 * @return 1 to go ahead, 0 if the user declined (or pressed ESC).
 */
static int bufferConfirmKill(struct buffer_t *buf) {
  const char *question = NULL;
  char name[256];
  char *name_str;
  char *c;
  char prompt[512];
  const char *u;
  Utf8Char *answer;
  int yes;
  if (buf->type == FILE_BUFF && buf->dirty)
    question = "Buffer %u modified; kill anyway? (y/n): %u";
  else if (buf->type == TERMINAL_BUFF && os_pty_alive(buf->pty))
    question = "Terminal %u still running; kill it? (y/n): %u";
  if (!question)
    return 1;

  // editorPrompt() takes a single %u for the typed answer; substitute the
  // buffer's name into the question first.
  name_str = (char *)utf8ToString(buf->filename);
  snprintf(name, sizeof(name), "%s", name_str ? name_str : "");
  gfree(name_str);
  for (c = name; *c; c++) // the result is a format string: no stray %
    if (*c == '%')
      *c = '?';
  u = strstr(question, "%u");
  snprintf(prompt, sizeof(prompt), "%.*s%s%s", (int)(u - question), question,
           name, u + 2);

  answer = editorPrompt(prompt, NULL, 0);
  yes = answer && answer->length == 1 &&
        (answer->character[0] == 'y' || answer->character[0] == 'Y');
  gfree(answer);
  if (!yes)
    editorSetStatusMessage("Kill cancelled");
  return yes;
}

/**
 * @brief Kills a buffer: every pane showing it moves to another buffer,
 * then the buffer and everything it owns is freed.
 * @details Follows Emacs' kill-buffer: panes aren't closed, they switch to
 * another buffer (a file buffer if there is one). The exception is the
 * explorer-style sidebar — a two-pane layout where the killed buffer is a
 * scratch buffer next to a real one — which collapses to the other pane
 * rather than leaving a file squeezed into the sidebar's width. Asks first
 * for a modified file or a live terminal (bufferConfirmKill()), refuses to
 * kill the very last buffer (every pane must always show one), closes the
 * document on the language server, and fires the "buffer-killed" hook
 * (name buffer-id) once the buffer is gone so packages holding its id —
 * the explorer's sidebar state, say — can drop it.
 * @param buff_id The buffer ID to kill
 * @return 0 on success, -1 if not found, refused, or cancelled
 */
int bufferClose(int buff_id) {
  struct buffer_t *buf = bufferFindById(buff_id);
  LspClient *lsp;
  int other;
  struct buffer_t *other_buf;
  struct buffer_t *replacement;
  struct buffer_t *b;
  int i;
  DiagnosticList *diags;
  int kept;
  char *short_str;
  char *name_str;
  Lisp hook_args;
  if (buf == NULL) {
    editorSetStatusMessage("Error: buffer not found");
    return -1;
  }
  if (buf->next == buf) {
    editorSetStatusMessage("Can't kill the only buffer");
    return -1;
  }
  if (buf->prompt_holds > 0) {
    editorSetStatusMessage("Another window has a prompt open on this buffer");
    return -1;
  }
  if (!bufferConfirmKill(buf))
    return -1;
  // Confirmed: the unsaved changes are meant to go, copy included.
  autosaveDiscard(buf);

  // Sidebar case: collapse to the other pane instead of refilling this one.
  if (buf->type == EXPLORER_BUFF && E.layout.num_panes == 2) {
    other = E.layout.panes[0].buffer_id == buff_id
                ? E.layout.panes[1].buffer_id
                : E.layout.panes[0].buffer_id;
    other_buf = bufferFindById(other);
    if (other != buff_id && other_buf && other_buf->type != EXPLORER_BUFF)
      splitScreenUnify(); // keeps the first non-EXPLORER_BUFF pane
  }

  // Prefer a real, editable file over the read-only splash screen or a
  // scratch/terminal buffer; fall back to anything else that's left.
  replacement = NULL;
  for (b = buf->next; b != buf && !replacement; b = b->next)
    if (b->type == FILE_BUFF && b->state != READ_ONLY)
      replacement = b;
  for (b = buf->next; b != buf && !replacement; b = b->next)
    if (b->type == FILE_BUFF)
      replacement = b;
  if (!replacement)
    replacement = buf->next;

  for (i = 0; i < E.layout.num_panes; i++) {
    if (E.layout.panes[i].buffer_id != buff_id)
      continue;
    if (i == E.layout.active_pane)
      bufferSwitch(replacement->buffer_id); // also resyncs the key prefix
    else
      E.layout.panes[i].buffer_id = replacement->buffer_id;
  }

  // Language server: close the document unless another buffer still has
  // the same file open, and drop its diagnostics — buffer ids are never
  // reused, so nothing would ever replace them.
  if (buf->b_lsp_open) {
    lsp = lspClientForBuffer(buf);
    if (bufferFindOtherLspOpen(buf) == -1 && lsp && lsp->state == LSP_READY)
      lspDidClose(lsp, buf);
    buf->b_lsp_open = 0;
  }
  diags = &E.lsp_diagnostics;
  kept = 0;
  for (i = 0; i < diags->count; i++)
    if (diags->entries[i].buffer_id != buff_id)
      diags->entries[kept++] = diags->entries[i];
  diags->count = kept;

  if (buf->type == TERMINAL_BUFF)
    ptySessionClose(buf);

  // Short name for the status bar, full path for the hook (same as
  // "buffer-opened" passes).
  short_str = (char *)utf8ToString(buf->filename);
  name_str =
      (char *)utf8ToString(buf->fullname ? buf->fullname : buf->filename);

  // Unlink before freeing, and move the list head off it if needed.
  if (E.buffers == buf)
    E.buffers = buf->next;
  buf->next->prev = buf->prev;
  buf->prev->next = buf->next;
  E.number_of_buffer--;

  bufferFree(buf);

  editorSetStatusMessage("Killed %s", short_str ? short_str : "buffer");
  gfree(short_str);
  hook_args =
      lisp_cons(lisp_make_string2(name_str ? name_str : "", E.ctx),
                lisp_cons(lisp_make_int(buff_id), lisp_null(), E.ctx), E.ctx);
  gfree(name_str);
  runHooks("buffer-killed", hook_args);
  return 0;
}

/**
 * @brief Gets the current active buffer
 * @return Pointer to current buffer_t, NULL if none
 */
struct buffer_t *bufferGetCurrent(void) {
  EditorPane *active = splitScreenGetActivePane();
  return bufferFindById(active->buffer_id);
}

/**
 * @brief Finds the registered lsp-server config matching a filename's
 * extension (see the Lisp `lsp-server` builtin), or NULL if none was
 * registered for it.
 * @note A buffer with no name at all (EXPLORER_BUFF: the explorer sidebar,
 * "*search*", "*tree*" — legitimately NULL fullname) has no extension and
 * so no server; utf8FilenameExtension() reports that rather than
 * dereferencing the NULL.
 */
struct lspServerConfig_t *lspServerConfigFor(Utf8Char *filename) {
  char ext[16];
  int i;
  if (!utf8FilenameExtension(filename, ext, sizeof(ext)))
    return NULL;

  for (i = 0; i < E.number_of_lsp_servers; i++)
    if (strcmp(E.lsp_servers[i].extension, ext) == 0)
      return &E.lsp_servers[i];
  return NULL;
}

/**
 * @brief The client for the server `cfg` names, or NULL if that server
 * hasn't been asked about yet this session.
 * @details One client per server command, not per lsp-server entry: a
 * server registered for several extensions (typescript-language-server for
 * ts and js, Angular's for ts and html) is one process for all of them.
 */
LspClient *lspClientForConfig(const struct lspServerConfig_t *cfg) {
  int i;
  if (!cfg || !cfg->argv[0])
    return NULL;
  for (i = 0; i < E.number_of_lsp_clients; i++)
    if (strcmp(E.lsp_clients[i]->command, cfg->argv[0]) == 0)
      return E.lsp_clients[i];
  return NULL;
}

/**
 * @brief The client `buf`'s file belongs to (by its extension's
 * lsp-server), whatever its state, or NULL.
 * @note The type check stays first: only a FILE_BUFF is guaranteed a
 * fullname (a scratch EXPLORER_BUFF has it NULL by design).
 */
LspClient *lspClientForBuffer(struct buffer_t *buf) {
  if (!buf || buf->type != FILE_BUFF || !E.constantes.LSP)
    return NULL;
  return lspClientForConfig(lspServerConfigFor(buf->fullname));
}

/**
 * @brief The client of the buffer in the active pane, or NULL.
 */
LspClient *lspActiveClient(void) {
  EditorPane *active = splitScreenGetActivePane();
  return active ? lspClientForBuffer(bufferFindById(active->buffer_id)) : NULL;
}

/**
 * @brief A new, not yet started client for `cfg`'s server, registered in
 * E.lsp_clients. NULL when LSP_MAX_CLIENTS servers are already known.
 */
static LspClient *lspNewClient(const struct lspServerConfig_t *cfg) {
  LspClient *lsp;
  if (E.number_of_lsp_clients >= LSP_MAX_CLIENTS)
    return NULL;
  lsp = (LspClient *)gcalloc(1, sizeof(LspClient));
  if (!lsp)
    return NULL;
  lsp->state = LSP_NOT_STARTED;
  lsp->proc = NULL;
  lsp->sync_debounce_buffer_id = -1;
  strncpy(lsp->language_id, cfg->language_id, sizeof(lsp->language_id) - 1);
  strncpy(lsp->command, cfg->argv[0], sizeof(lsp->command) - 1);
  E.lsp_clients[E.number_of_lsp_clients++] = lsp;
  return lsp;
}

/**
 * @brief The first time a file whose extension has a registered lsp-server
 * is opened, ask whether to start that server and where the project root
 * is. Asked once per server: each language gets its own server, running
 * side by side, and a decline (or ESC) leaves that server's client in
 * LSP_SHUTDOWN so it isn't asked about again this session.
 * @note Called before editorOpen() loads the buffer's content — it only
 * needs the filename, not the text. See bufferMaybeSyncLsp() for buffers
 * created *after* their server's already running, which does need the text
 * and so must run after editorOpen().
 */
void bufferMaybeStartLsp(struct buffer_t *buf) {
  struct lspServerConfig_t *cfg;
  LspClient *lsp;
  Utf8Char *answer;
  Utf8Char *default_root;
  Utf8Char *root;
  char *root_str;
  char question[256];
  if (!E.constantes.LSP)
    return;

  cfg = lspServerConfigFor(buf->filename);
  if (!cfg)
    return; // no server registered for this extension
  if (lspClientForConfig(cfg))
    return; // already asked about this server: running, starting or declined

  lsp = lspNewClient(cfg);
  if (!lsp) {
    editorSetStatusMessage("Too many language servers (%d) - not starting %s",
                           LSP_MAX_CLIENTS, cfg->argv[0]);
    return;
  }

  // The server's name in the question: with several languages it's no
  // longer obvious which one is meant. No % from the command: the
  // question is a format string.
  snprintf(question, sizeof(question),
           "Start the %s language server for this project? (y/n): %%u",
           strchr(cfg->argv[0], '%') ? "configured" : cfg->argv[0]);
  answer = editorPrompt(question, NULL, 0);
  if (!answer || answer->length != 1 ||
      (answer->character[0] != 'y' && answer->character[0] != 'Y')) {
    gfree(answer);
    lsp->state = LSP_SHUTDOWN; // declined — don't ask again this session
    return;
  }
  gfree(answer);

  default_root = buf->path ? buf->path : buf->fullname;
  root = editorPrompt("Project root: %u", default_root,
                      1); // bPathMode=1: Tab completes paths
  if (!root) {
    lsp->state = LSP_SHUTDOWN;
    return;
  }

  root_str = (char *)utf8ToString(root);
  gfree(root);

  if (lspStart(lsp, root_str, cfg->argv) != 0)
    editorSetStatusMessage("Failed to start %s", cfg->argv[0]);

  gfree(root_str);
}

/**
 * @brief Syncs a newly-opened buffer with its LSP server when that server
 * is already running (e.g. a second file opened after the first one
 * started it).
 * @note Must run after editorOpen() — it sends the buffer's actual text,
 * so the rows need to already be loaded. The one-time startup flow in
 * bufferMaybeStartLsp() runs *before* editorOpen() instead, since the
 * retroactive didOpen loop in lsp_on_initialize_response() (which fires
 * once the handshake completes, well after this buffer's own
 * bufferCreate() call has fully returned) covers the very first buffer.
 */
void bufferMaybeSyncLsp(struct buffer_t *buf) {
  LspClient *lsp = lspClientForBuffer(buf);
  if (!lsp || lsp->state != LSP_READY)
    return; // none, declined, or not ready yet — the retroactive didOpen
            // loop will cover it
  if (buf->b_lsp_open)
    return; // already synced

  // Same file already open (and synced) in another buffer — the server
  // only expects one didOpen per URI, so just piggyback on that
  // buffer's already-open document instead of sending a second one.
  if (bufferFindOtherLspOpen(buf) != -1) {
    buf->b_lsp_open = 1;
    buf->lsp_synced_seq = buf->edit_seq;
    return;
  }

  lspDidOpen(lsp, buf);
  buf->b_lsp_open = 1;
  buf->lsp_synced_seq = buf->edit_seq;
}

/**
 * @brief Lists all open buffers
 * @details Prints buffer information to status message
 * @return Number of open buffers
 */
int bufferListAll(void) {
  char buf[256] = "";
  int offset;
  EditorPane *active;
  int first_buffer_id;
  struct buffer_t *b;
  char marker;
  char *name;
  if (E.number_of_buffer == 0) {
    editorSetStatusMessage("No buffers open");
    return 0;
  }

  offset = 0;
  active = splitScreenGetActivePane();
  first_buffer_id = E.buffers->buffer_id;
  b = E.buffers;
  do {
    marker = (b->buffer_id == active->buffer_id) ? '*' : ' ';
    name = b->filename ? (char *)utf8ToString(b->filename) : NULL;
    offset += snprintf(&buf[offset], sizeof(buf) - offset, "%c%d:%s ", marker,
                       b->buffer_id, name ? name : "[No Name]");
    gfree(name);
    b = b->next;
  } while (b->buffer_id != first_buffer_id && offset < (int)sizeof(buf) - 1);
  editorSetStatusMessage("Buffers: %s", buf);
  return E.number_of_buffer;
}

/**
 * @brief Saves current buffer to disk
 * @return 0 on success, -1 on failure
 */
int bufferSave(void) {
  EditorPane *active = splitScreenGetActivePane();
  if (active->buffer_id == -1) {
    editorSetStatusMessage("Error: no buffer active");
    return -1;
  }

  editorSave();
  return 0;
}

/**
 * @brief Saves all buffers to disk
 * @details Iterates through all buffers, saves each one
 * @return 0 on success, -1 on failure
 */
int bufferSaveAll(void) {
  int saved = 0;
  int failed = 0;

  int first_buffer_id = E.buffers[0].buffer_id;
  struct buffer_t *b = E.buffers;

  do {
    if (bufferSwitch(b->buffer_id) == 0) {
      if (E.dirty && bufferSave() == 0) {
        saved++;
      }
    } else {
      failed++;
    }
    b = b->next;
  } while (b->buffer_id != first_buffer_id);

  editorSetStatusMessage("Saved %d buffers (%d failed)", saved, failed);
  return (failed == 0) ? 0 : -1;
}

/**
 * @brief Maps a byte offset into a row's utf8ToString() rendering back to
 * a character index into row->characters[] — needed because strstr()
 * matches are byte offsets, but buf->x is a character index, and the two
 * only coincide for pure-ASCII content.
 */
static int bufferByteOffsetToCharIndex(Row *row, int byte_offset) {
  int bytes = 0;
  int i;
  for (i = 0; i < row->size; i++) {
    if (bytes == byte_offset)
      return i;
    bytes += row->characters[i].length;
  }
  return row->size;
}

/**
 * @brief Finds the last occurrence of needle in haystack starting before
 * byte offset max_byte (exclusive) — strstr() only ever finds the first,
 * which is the wrong direction for a reverse search.
 */
static char *bufferFindLastMatchBefore(const char *haystack, const char *needle,
                                       int max_byte) {
  char *result = NULL;
  const char *search_from = haystack;
  char *match;
  while (1) {
    match = strstr(search_from, needle);
    if (!match || (int)(match - haystack) >= max_byte)
      break;
    result = match;
    search_from = match + 1;
  }
  return result;
}

/**
 * @brief Searches for a string in the document
 * @details Prompts user for a search query, then searches forward from current
 * cursor position (starting just after the cursor on the current line, so
 * repeated searches step to the next match instead of re-matching in
 * place). Updates cursor position to the first match found. Does not wrap
 * around past the end of the buffer.
 * @note Updates global editor state E (cursor position, row_offset)
 * @note Search is case-sensitive
 * @see editorPrompt()
 */
void bufferFind(struct buffer_t *buf) {
  Utf8Char *query;
  char *query_str;
  int i;
  Row *row;
  char *row_str;
  int start_char;
  int start_byte;
  int c;
  int row_bytes;
  char *match;
  appDebug("searching\n");
  query = editorPrompt("Search: %u (ESC to cancel)", NULL, 0);
  if (!query)
    return;

  query_str = (char *)utf8ToString(query);
  gfree(query);
  if (query_str[0] == '\0') {
    gfree(query_str);
    return;
  }

  for (i = buf->y; i < buf->numrows; i++) {
    row = &buf->row[i];
    row_str = (char *)utf8ToString(row->characters);

    start_char = (i == buf->y) ? buf->x + 1 : 0;
    start_byte = 0;
    for (c = 0; c < start_char && c < row->size; c++)
      start_byte += row->characters[c].length;
    row_bytes = (int)strlen(row_str);
    if (start_byte > row_bytes)
      start_byte = row_bytes;

    match = strstr(row_str + start_byte, query_str);
    if (match) {
      buf->y = i;
      buf->x = bufferByteOffsetToCharIndex(row, (int)(match - row_str));
      gfree(row_str);
      break;
    }
    gfree(row_str);
  }
  gfree(query_str);
}

/**
 * @brief Searches backward for a string in the document
 * @details Mirrors bufferFind() but scans from the current cursor position
 * back toward the start of the buffer, stopping at the last match before
 * the cursor on the current line. Does not wrap around past the start.
 * @see bufferFind()
 */
void bufferFindReverse(struct buffer_t *buf) {
  Utf8Char *query;
  char *query_str;
  int i;
  Row *row;
  char *row_str;
  int end_char;
  int end_byte;
  int c;
  char *match;
  appDebug("searching\n");
  query = editorPrompt("Reverse search: %u (ESC to cancel)", NULL, 0);
  if (!query)
    return;

  query_str = (char *)utf8ToString(query);
  gfree(query);
  if (query_str[0] == '\0') {
    gfree(query_str);
    return;
  }

  for (i = buf->y; i >= 0; i--) {
    row = &buf->row[i];
    row_str = (char *)utf8ToString(row->characters);

    end_char = (i == buf->y) ? buf->x : row->size;
    end_byte = 0;
    for (c = 0; c < end_char && c < row->size; c++)
      end_byte += row->characters[c].length;

    match = bufferFindLastMatchBefore(row_str, query_str, end_byte);
    if (match) {
      buf->y = i;
      buf->x = bufferByteOffsetToCharIndex(row, (int)(match - row_str));
      gfree(row_str);
      break;
    }
    gfree(row_str);
  }
  gfree(query_str);
}

/**
 * @brief Whether a cell belongs to a word for buffer-find-word: ASCII
 * letters, digits and '_' (so identifiers like my_var match whole), plus
 * any multi-byte UTF-8 character, so accented words aren't cut in two.
 */
static int bufferIsWordChar(const Utf8Char *ch) {
  unsigned char c;
  if (ch->length > 1)
    return 1;
  c = ch->character[0];
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static int bufferCharEqual(const Utf8Char *a, const Utf8Char *b) {
  return a->length == b->length &&
         memcmp(a->character, b->character, (size_t)a->length) == 0;
}

/**
 * @brief Whether the n characters of word sit at column col of row as a
 * whole word (not preceded or followed by another word character).
 */
static int bufferWordMatchAt(const Row *row, int col, const Utf8Char *word,
                             int n) {
  int i;
  if (col < 0 || col + n > row->size)
    return 0;
  if (col > 0 && bufferIsWordChar(&row->characters[col - 1]))
    return 0;
  if (col + n < row->size && bufferIsWordChar(&row->characters[col + n]))
    return 0;
  for (i = 0; i < n; i++)
    if (!bufferCharEqual(&row->characters[col + i], &word[i]))
      return 0;
  return 1;
}

/**
 * @brief Moves the cursor to the next (or previous) whole-word occurrence
 * of the word under the cursor, like Vim's * and #
 * @details When the cursor isn't on a word, the first word after it on the
 * same line is used. The search is case-sensitive, wraps around the ends of
 * the buffer, and lands on the start of the match. The status bar says when
 * the search wrapped or when the word has no other occurrence.
 * @param buf Buffer to search in
 * @param reverse 0 to search forward, 1 to search backward
 */
void bufferFindWordAtCursor(struct buffer_t *buf, int reverse) {
  Row *row;
  int start;
  int end;
  int n;
  int k;
  int r;
  int c;
  int from;
  int to;
  char *word_str;

  if (!buf || buf->numrows == 0 || buf->y >= buf->numrows)
    return;
  row = &buf->row[buf->y];

  start = buf->x;
  while (start < row->size && !bufferIsWordChar(&row->characters[start]))
    start++;
  if (start >= row->size) {
    editorSetStatusMessage("No word under cursor");
    return;
  }
  while (start > 0 && bufferIsWordChar(&row->characters[start - 1]))
    start--;
  end = start;
  while (end < row->size && bufferIsWordChar(&row->characters[end]))
    end++;
  n = end - start;

  /* k = 0 is the rest of the cursor's row past the word's start, k = 1 ..
   * numrows - 1 the other rows in search order, and k = numrows the
   * cursor's row again from the other side, which ends on the word itself. */
  for (k = 0; k <= buf->numrows; k++) {
    if (reverse)
      r = ((buf->y - k) % buf->numrows + buf->numrows) % buf->numrows;
    else
      r = (buf->y + k) % buf->numrows;
    row = &buf->row[r];

    if (reverse) {
      from = (k == 0) ? start - 1 : row->size - n;
      to = (k == buf->numrows) ? start : 0;
      for (c = from; c >= to; c--)
        if (bufferWordMatchAt(row, c, buf->row[buf->y].characters + start, n))
          goto found;
    } else {
      from = (k == 0) ? start + 1 : 0;
      to = (k == buf->numrows) ? start : row->size - n;
      for (c = from; c <= to; c++)
        if (bufferWordMatchAt(row, c, buf->row[buf->y].characters + start, n))
          goto found;
    }
  }
  return;

found:
  if (r == buf->y && c == start) {
    word_str = (char *)utf8ToNString(buf->row[buf->y].characters + start, n);
    editorSetStatusMessage("Only occurrence of \"%s\"", word_str);
    gfree(word_str);
  } else if (reverse ? (r > buf->y || (r == buf->y && c > start))
                     : (r < buf->y || (r == buf->y && c < start))) {
    editorSetStatusMessage(reverse ? "Search wrapped to the bottom"
                                   : "Search wrapped to the top");
  }
  buf->y = r;
  buf->x = c;
}



/**
 * @brief Index of `row` within `buffer->row`, or -1 if it isn't one of its
 * rows — -1 makes syntaxHighlighterInvalidateAfter() drop the whole cache,
 * the safe answer when we can't tell which row changed.
 */
static int rowCapacity(int numrows) {
  int cap = 16;
  while (cap < numrows)
    cap *= 2;
  return cap;
}

static int rowIndexOf(const struct buffer_t *buffer, const Row *row) {
  if (!buffer->row || row < buffer->row || row >= buffer->row + buffer->numrows)
    return -1;
  return (int)(row - buffer->row);
}

void bufferInsertRow(struct buffer_t *buffer, int at, Utf8Char *string,
                     int len) {
  int needed;
  Row *tmp;
  if (at < 0 || at > buffer->numrows)
    return;
  // Grow by doubling instead of one row at a time (which made loading an
  // n-line file O(n) reallocs). No capacity field needed: this is the only
  // place ->row ever grows, it never shrinks, and every other site resets
  // it to NULL — so the array always holds at least rowCapacity(numrows).
  needed = buffer->numrows + 1;
  if (!buffer->row || needed > rowCapacity(buffer->numrows)) {
    tmp =
        (Row *)grealloc(buffer->row, sizeof(Row) * (size_t)rowCapacity(needed));
    if (!tmp)
      return;
    buffer->row = tmp;
  }

  /* Shift existing rows to make room at 'at' — no at++ */
  if (at < buffer->numrows) {
    memmove(&buffer->row[at + 1], &buffer->row[at],
            sizeof(Row) * (buffer->numrows - at)); /* not -at+1 */
  }

  buffer->row[at].size = len;
  buffer->row[at].characters = gcalloc((len + 1), sizeof(Utf8Char));
  buffer->row[at].attrs = NULL;

  if (!buffer->row[at].characters)
    return;
  memcpy(buffer->row[at].characters, string, len * sizeof(Utf8Char));
  /* always NUL-terminate */
  END_OF_STRING(buffer->row[at].characters[len]);

  buffer->numrows++;
  if (buffer->type != TERMINAL_BUFF) {
    buffer->dirty++;
    buffer->edit_seq++;
  }
  // Rows appended at the end (a file being read) move nothing.
  if (at < buffer->numrows - 1) {
    breakpointsLinesInserted(buffer, at, 1);
    splitScreenLinesShifted(buffer, at, 1);
  }
  syntaxHighlighterInvalidateAfter(buffer, at - 1);
}

/**
 * @brief Makes room for `count` empty rows at `at` with one move of the row
 * array (bufferInsertRow() moves it once per row). Pure row mutation.
 */
static int rowsInsertEmpty(struct buffer_t *buf, int at, int count) {
  Row *tmp;
  int i;
  if (count <= 0)
    return 1;
  if (!buf->row || buf->numrows + count > rowCapacity(buf->numrows)) {
    tmp = (Row *)grealloc(
        buf->row, sizeof(Row) * (size_t)rowCapacity(buf->numrows + count));
    if (!tmp)
      return 0;
    buf->row = tmp;
  }
  memmove(&buf->row[at + count], &buf->row[at],
          sizeof(Row) * (size_t)(buf->numrows - at));
  for (i = at; i < at + count; i++) {
    buf->row[i].size = 0;
    buf->row[i].attrs = NULL;
    buf->row[i].characters = gcalloc(1, sizeof(Utf8Char));
    END_OF_STRING(buf->row[i].characters[0]);
  }
  buf->numrows += count;
  if (at < buf->numrows - count) {
    breakpointsLinesInserted(buf, at, count);
    splitScreenLinesShifted(buf, at, count);
  }
  return 1;
}

/**
 * @brief Inserts `n` characters into `r` at `at`. Pure row mutation.
 */
static void rowInsertChars(Row *r, int at, const Utf8Char *chars, int n) {
  Utf8Char *grown;
  if (n <= 0)
    return;
  grown = grealloc(r->characters, (size_t)(r->size + n + 1) * sizeof(Utf8Char));
  if (!grown)
    return;
  r->characters = grown;
  memmove(r->characters + at + n, r->characters + at,
          (size_t)(r->size - at) * sizeof(Utf8Char));
  memcpy(r->characters + at, chars, (size_t)n * sizeof(Utf8Char));
  r->size += n;
  END_OF_STRING(r->characters[r->size]);
}

/**
 * @brief Inserts UTF-8 `text` ('\n' between lines) at (row, col) without
 * recording undo, and reports where it ends.
 * @details The rows the text needs are made in one go and filled
 * directly, so a large paste costs about as much as loading the same text
 * from a file.
 */
static void textInsertRaw(struct buffer_t *buf, int row, int col,
                          const char *text, int *end_row, int *end_col) {
  const unsigned char *p;
  Utf8Char *chars;
  Utf8Char *tail;
  int tail_len;
  int n;
  int lines;
  int line_start;
  int i;
  int cur;
  Row *r;

  *end_row = row;
  *end_col = col;
  if (!text || !text[0])
    return;
  if (buf->numrows == 0)
    rowInsertBlank(buf, 0);
  if (row >= buf->numrows)
    row = buf->numrows - 1;
  if (col > buf->row[row].size)
    col = buf->row[row].size;

  // Decode once; '\n' stays in the array as a line marker.
  chars = galloc((strlen(text) + 1) * sizeof(Utf8Char));
  if (!chars)
    return;
  n = 0;
  lines = 0;
  for (p = (const unsigned char *)text; *p;) {
    if (*p == '\n') {
      chars[n].character[0] = '\n';
      chars[n].length = 0; // marker, never stored in a row
      n++;
      lines++;
      p++;
    } else {
      p += utf8CreateChar(p, &chars[n++]);
    }
  }

  r = &buf->row[row];
  if (lines == 0) {
    rowInsertChars(r, col, chars, n);
    *end_row = row;
    *end_col = col + n;
    gfree(chars);
    syntaxHighlighterInvalidateAfter(buf, row);
    return;
  }

  // Split off what follows the insertion point; it ends up after the
  // inserted text's last line.
  tail_len = r->size - col;
  tail = galloc((size_t)(tail_len + 1) * sizeof(Utf8Char));
  if (!tail || !rowsInsertEmpty(buf, row + 1, lines)) {
    gfree(tail);
    gfree(chars);
    return;
  }
  r = &buf->row[row];
  memcpy(tail, r->characters + col, (size_t)tail_len * sizeof(Utf8Char));
  r->size = col;
  END_OF_STRING(r->characters[r->size]);
  if (rowHeadIsBlank(r, col))
    breakpointsLineMoved(buf, row, row + lines);

  cur = row;
  line_start = 0;
  for (i = 0; i <= n; i++) {
    if (i == n || chars[i].length == 0) {
      r = &buf->row[cur];
      rowInsertChars(r, r->size, chars + line_start, i - line_start);
      line_start = i + 1;
      if (i < n)
        cur++;
    }
  }
  r = &buf->row[cur];
  *end_row = cur;
  *end_col = r->size;
  rowInsertChars(r, r->size, tail, tail_len);
  gfree(tail);
  gfree(chars);
  syntaxHighlighterInvalidateAfter(buf, row);
}

/**
 * @brief Deletes [start_row:start_col, end_row:end_col) without recording
 * undo and returns the deleted text (caller frees). The range is clamped
 * to the buffer.
 */
static char *textDeleteRaw(struct buffer_t *buf, int start_row, int start_col,
                           int end_row, int end_col) {
  char *deleted;
  Row *first;
  Row *last;
  int i;
  int removed;
  if (start_row >= buf->numrows)
    return NULL;
  if (end_row >= buf->numrows) {
    end_row = buf->numrows - 1;
    end_col = buf->row[end_row].size;
  }
  if (start_col > buf->row[start_row].size)
    start_col = buf->row[start_row].size;
  if (end_col > buf->row[end_row].size)
    end_col = buf->row[end_row].size;
  if (end_row < start_row || (end_row == start_row && end_col <= start_col))
    return NULL;

  deleted = bufferGetRegionText(buf, start_row, start_col, end_row, end_col);
  first = &buf->row[start_row];
  if (start_row == end_row) {
    memmove(first->characters + start_col, first->characters + end_col,
            (size_t)(first->size - end_col) * sizeof(Utf8Char));
    first->size -= end_col - start_col;
    END_OF_STRING(first->characters[first->size]);
  } else {
    last = &buf->row[end_row];
    first->size = start_col;
    rowInsertChars(first, start_col, last->characters + end_col,
                   last->size - end_col);
    if (first->size == start_col)
      END_OF_STRING(first->characters[first->size]);
    removed = end_row - start_row;
    for (i = start_row + 1; i <= end_row; i++)
      bufferFreeRow(&buf->row[i]);
    memmove(&buf->row[start_row + 1], &buf->row[end_row + 1],
            sizeof(Row) * (size_t)(buf->numrows - end_row - 1));
    buf->numrows -= removed;
    breakpointsLinesRemoved(buf, start_row + 1, removed);
    splitScreenLinesShifted(buf, start_row + 1, -removed);
  }
  syntaxHighlighterInvalidateAfter(buf, start_row);
  return deleted;
}

/**
 * @brief Frees a buffer and everything it owns — rows, names, undo/redo
 * history, git line status, comment-state cache — once it's unlinked.
 * @details The one place that knows what a buffer owns: bufferClose()
 * (CTRL-x k) and the teardown at exit (bFree_structs()) both use it. They
 * used to keep a list each, and the exit one fell behind, leaving every
 * buffer's undo history, git status and comment cache allocated at exit.
 * Doesn't unlink, close a terminal, or tell the language server — that's
 * bufferClose()'s job.
 */
void bufferFree(struct buffer_t *buf) {
  int i;
  if (!buf)
    return;
  if (buf->type == TERMINAL_BUFF)
    ansiParserRelease(buf);
  for (i = 0; i < buf->numrows; i++)
    bufferFreeRow(&buf->row[i]);
  gfree(buf->row);
  gfree(buf->filename);
  gfree(buf->path);
  gfree(buf->fullname);
  undoListClear(&buf->undo_stack);
  undoListClear(&buf->redo_stack);
  gfree(buf->undo_stack.entries);
  gfree(buf->redo_stack.entries);
  gfree(buf->git_lines);
  lspFreeSymbols(buf->lsp_symbols, buf->lsp_symbols_count);
  lspFreeInlayHints(buf->lsp_hints, buf->lsp_hints_count);
  lspFreeInlayHints(buf->lsp_layout, buf->lsp_layout_count);
  gfree(buf->comment_state);
  gfree(buf->lsp_sent_text);
  gfree(buf);
}

void bufferFreeRow(Row *row) {
  gfree(row->characters);
  gfree(row->attrs);
}

/**
 * \fn editorRowInsertChar(erow *row, int at, int c)
 * \param buffer The buffer `row` belongs to
 * \param row Row to insert into
 * \param at Index of where we want to insert the char
 * \param character The character to insert */

void bufferRowInsertChar(struct buffer_t *buffer, Row *row, int at,
                         Utf8Char *character) {
  Utf8Char *new_chars;
  if (buffer->state == READ_ONLY)
    return;
  if (at < 0 || at > row->size)
    return;

  new_chars = (Utf8Char *)grealloc(
      row->characters,
      (row->size + 2) * sizeof(Utf8Char)); // +1 new char, +1 terminator
  if (!new_chars) {
    return;
  }
  row->characters = new_chars;

  memmove(row->characters + at + 1, row->characters + at,
          (row->size - at) * sizeof(Utf8Char));

  memcpy(row->characters + at, character, sizeof(Utf8Char));
  row->size += 1;

  END_OF_STRING(row->characters[row->size]);
  if (buffer->type != TERMINAL_BUFF) {
    ++buffer->dirty;
    buffer->edit_seq++;
  }
  syntaxHighlighterInvalidateAfter(buffer, rowIndexOf(buffer, row));
}

/**
 * \fn bufferRowDelChar(struct bufferConfig *E, frow *frow, int at)
 * \brief Delete the a char at the chosen position on the given row
 * \param buffer The buffer `row` belongs to
 * \param row Row on operation is made
 * \param at Index of the char to delete
 * \note Pure row mutation — does not touch buffer->x. It used to
 * unconditionally decrement it (assuming every caller was a backspace at
 * the cursor), which made this unusable as a general "delete at column N"
 * primitive for anything else (undo replay, forward-delete, region kill).
 * Callers that want the cursor to follow (like bufferDelChar's backspace
 * path) move it themselves. */
void bufferRowDelChar(struct buffer_t *buffer, Row *row, int at) {
  if (buffer->state == READ_ONLY)
    return;
  if (at < 0 || at >= row->size)
    return;
  memmove(row->characters + at, row->characters + (at + 1),
          (row->size - at - 1) * sizeof(Utf8Char));
  row->size -= 1;
  END_OF_STRING(row->characters[row->size]);
  syntaxHighlighterInvalidateAfter(buffer, rowIndexOf(buffer, row));
}

void bufferInsertChar(Utf8Char *character) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  Utf8Char end_string;
  int col;
  // A terminal buffer's rows are owned by the ANSI parser (ansi_parser.c),
  // which keeps its own attrs[] array in lockstep with row->size — this
  // generic path has no idea attrs[] even exists, so growing row->size
  // through here leaves it undersized relative to what termRenderRow()
  // later reads. That's reachable whenever E.prefix_state doesn't
  // actually match the focused pane's buffer type (e.g. ESCAPE out of the
  // "term" keymap while still looking at the terminal, or a pane
  // collapse landing on it) and ENTER/typing/etc. run as ordinary edits
  // instead of being forwarded to the PTY — a real heap-buffer-overflow,
  // not just wrong-looking output. Ordinary keystrokes belong to the
  // shell via ptyForwardKey(); nothing here should ever touch the row
  // directly, regardless of which prefix let the call through.
  if (buf->type == TERMINAL_BUFF)
    return;
  if (buf->y == buf->numrows) {
    END_OF_STRING(end_string);
    bufferInsertRow(buf, buf->numrows, &end_string, 0);
  }
  col = buf->x;
  bufferRowInsertChar(buf, &buf->row[buf->y], col, character);
  undoRecord(buf, UNDO_INSERT_CHAR, buf->y, col, *character);
  buf->x = col + 1;
  buf->b_has_changed = 1;
}

void bufferDelChar(void) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  Row *r;
  int col;
  Utf8Char deleted;
  int target_row;
  int prev_char_count;

  // See bufferInsertChar()'s comment — same reasoning, same risk.
  if (buf->type == TERMINAL_BUFF)
    return;

  /* Nothing to delete */
  if (buf->numrows == 0)
    return;
  if (buf->x == 0 && buf->y == 0)
    return;

  /* Use row_offset, not col_offset, for row indexing */
  r = &buf->row[buf->y];

  if (buf->x > 0) {
    col = buf->x - 1;
    deleted = r->characters[col];
    bufferRowDelChar(buf, r, col);
    undoRecord(buf, UNDO_DELETE_CHAR, buf->y, col, deleted);
    buf->x = col;
  } else {
    /* Merge current row into the previous one */
    target_row = buf->y - 1;
    prev_char_count = buf->row[target_row].size;
    rowJoinNext(buf, target_row);
    undoRecord(buf, UNDO_JOIN_LINE, target_row, prev_char_count, (Utf8Char){0});

    active->cursor_x = utf8ColumnOf(buf->row[target_row].characters,
                                    buf->row[target_row].size, prev_char_count);
    buf->x = prev_char_count;
    active->cursor_y--;
    buf->y--;
  }
  if (buf->type != TERMINAL_BUFF) {
    buf->dirty++;
    buf->edit_seq++;
  }
}

void bufferInsertNewLine(void) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  int split_row;
  int split_col;

  // Unlike character insert/delete (bufferRowInsertChar/bufferRowDelChar),
  // this one never checked READ_ONLY at all — harmless for a normal
  // read-only file buffer (ENTER isn't a destructive edit to undo), but
  // an EXPLORER_BUFF's rows are meant to be set only by Lisp
  // (editor-scratch-set-lines), never mutated from the keyboard.
  if (buf->state == READ_ONLY)
    return;
  // TERMINAL_BUFF is READ_AND_WRITE (the shell writes to it constantly),
  // so the check above doesn't cover it — see bufferInsertChar()'s
  // comment for why splitting a terminal's row here is a real
  // memory-safety bug, not just a display one.
  if (buf->type == TERMINAL_BUFF)
    return;

  if (buf->y >= buf->numrows) {
    /* Cursor is past the last row: just append a blank line */
    rowInsertBlank(buf, buf->numrows);
    undoRecord(buf, UNDO_INSERT_ROW, buf->numrows - 1, 0, (Utf8Char){0});
  } else {
    split_row = buf->y;
    split_col = buf->x;
    rowSplitAt(buf, split_row, split_col);
    undoRecord(buf, UNDO_SPLIT_LINE, split_row, split_col, (Utf8Char){0});
  }
  buf->y++;
  buf->x = 0;
}

char *bufferToText(struct buffer_t *buf) {
  size_t total = 0;
  int i;
  Row *row;
  int j;
  char *text;
  char *p;
  for (i = 0; i < buf->numrows; i++) {
    row = &buf->row[i];
    for (j = 0; j < row->size; j++)
      total += row->characters[j].length;
    total += 1; // '\n'
  }

  text = galloc(total + 1);
  if (!text)
    return NULL;

  p = text;
  for (i = 0; i < buf->numrows; i++) {
    row = &buf->row[i];
    for (j = 0; j < row->size; j++) {
      memcpy(p, row->characters[j].character, row->characters[j].length);
      p += row->characters[j].length;
    }
    *p++ = '\n';
  }
  *p = '\0';
  return text;
}
