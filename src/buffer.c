/**
 * @file buffer.c
 * @brief Buffer management implementation for multiple open files
 */

#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include "../include/split_screen.h"
#include "../include/utils.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../include/append_buffer.h"
#include "../include/builtins.h"
#include "../include/completion.h"
#include "../include/input.h"
#include "../include/pty_session.h"
#include "../include/subprocess.h"
#include "../include/syntax_highlighter.h"
#include "../include/terminal.h"
#include "../include/utf8.h"

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
static void undoListPush(UndoList *list, UndoEntry entry) {
  if (list->count >= UNDO_MAX_ENTRIES) {
    memmove(list->entries, list->entries + 1,
            sizeof(UndoEntry) * (list->count - 1));
    list->count--;
  }
  if (list->count >= list->capacity) {
    int new_cap = list->capacity ? list->capacity * 2 : 64;
    UndoEntry *grown = realloc(list->entries, sizeof(UndoEntry) * new_cap);
    if (!grown)
      return; // best-effort: drop the entry rather than crash
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
static void undoRecord(struct buffer_t *buf, UndoKind kind, int row, int col,
                       Utf8Char ch) {
  buf->redo_stack.count = 0;

  int contiguous = 0;
  if (buf->undo_group_open && buf->undo_last_kind == kind &&
      buf->undo_last_row == row) {
    if (kind == UNDO_INSERT_CHAR && col == buf->undo_last_col + 1)
      contiguous = 1; // typing forward
    else if (kind == UNDO_DELETE_CHAR &&
             (col == buf->undo_last_col - 1 || col == buf->undo_last_col))
      contiguous = 1; // backspacing (col-1) or forward-deleting at a fixed col
  }
  if (!contiguous)
    buf->undo_next_group_id++;

  UndoEntry entry = {.kind = kind,
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

/**
 * @brief Splits `row` into two rows at character index `col` — the tail
 * ([col, size)) becomes a new row right after it. Pure row mutation: no
 * cursor movement, no undo recording (callers do both themselves).
 */
static void rowSplitAt(struct buffer_t *buf, int row, int col) {
  Row *r = &buf->row[row];
  bufferInsertRow(buf, row + 1, &r->characters[col], r->size - col);
  r = &buf->row[row]; // bufferInsertRow's recalloc may have moved the array
  r->size = col;
  END_OF_STRING(r->characters[r->size]);
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
  for (int i = 0; i < next->size; ++i)
    bufferRowInsertChar(buf, dest, dest->size, &next->characters[i]);
  free(next->characters);
  next->characters = NULL;
  memmove(&buf->row[row + 1], &buf->row[row + 2],
          sizeof(Row) * (buf->numrows - row - 2));
  buf->numrows--;
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
  syntaxHighlighterInvalidate(buf);
}

/**
 * @brief Undoes the most recent group of edits on `buf` (one atomic edit,
 * or a coalesced run of contiguous ones — see undoRecord()), moving each
 * entry onto the redo stack unchanged so bufferRedo() can replay it
 * forward again. Leaves the cursor at the last-undone edit's position.
 */
void bufferUndo(struct buffer_t *buf) {
  if (buf->undo_stack.count == 0) {
    editorSetStatusMessage("No further undo information");
    return;
  }

  int gid = buf->undo_stack.entries[buf->undo_stack.count - 1].group_id;
  while (buf->undo_stack.count > 0 &&
         buf->undo_stack.entries[buf->undo_stack.count - 1].group_id == gid) {
    UndoEntry e = buf->undo_stack.entries[--buf->undo_stack.count];
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
    }
    undoListPush(&buf->redo_stack, e);
    buf->x = e.col;
    buf->y = e.row;
  }
  buf->undo_group_open =
      0; // the next fresh edit must not merge into what's left
  if (buf->type != TERMINAL_BUFF)
    buf->dirty++;
  editorSetStatusMessage("Undo");
}

/**
 * @brief Redoes the most recently undone group of edits on `buf`, moving
 * each entry back onto the undo stack unchanged. Leaves the cursor at the
 * last-redone edit's position.
 */
void bufferRedo(struct buffer_t *buf) {
  if (buf->redo_stack.count == 0) {
    editorSetStatusMessage("No further redo information");
    return;
  }

  int gid = buf->redo_stack.entries[buf->redo_stack.count - 1].group_id;
  while (buf->redo_stack.count > 0 &&
         buf->redo_stack.entries[buf->redo_stack.count - 1].group_id == gid) {
    UndoEntry e = buf->redo_stack.entries[--buf->redo_stack.count];
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
    }
    undoListPush(&buf->undo_stack, e);
    buf->x = e.col;
    buf->y = e.row;
  }
  buf->undo_group_open = 0;
  if (buf->type != TERMINAL_BUFF)
    buf->dirty++;
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
 * plain '\n'-joined, NUL-terminated C string. Caller must free() it.
 */
char *bufferGetRegionText(struct buffer_t *buf, int start_row, int start_col,
                          int end_row, int end_col) {
  size_t cap = 256, len = 0;
  char *text = malloc(cap);
  if (!text)
    return NULL;

  for (int row = start_row; row <= end_row && row < buf->numrows; row++) {
    Row *r = &buf->row[row];
    int from = (row == start_row) ? start_col : 0;
    int to = (row == end_row) ? end_col : r->size;
    if (to > r->size)
      to = r->size;

    for (int c = from; c < to; c++) {
      Utf8Char *ch = &r->characters[c];
      if (len + (size_t)ch->length + 2 > cap) {
        cap *= 2;
        char *grown = realloc(text, cap);
        if (!grown) {
          free(text);
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
        char *grown = realloc(text, cap);
        if (!grown) {
          free(text);
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
 * @brief Deletes [start_row:start_col, end_row:end_col) from `buf`, one
 * character (or line join) at a time through the same undo-recorded
 * primitives as normal editing — so a region delete is undoable, just
 * possibly as more than one group if it spans a line boundary (each
 * UNDO_JOIN_LINE always starts a fresh group; see undoRecord()). Leaves
 * the cursor at the deletion's start.
 */
void bufferDeleteRegion(struct buffer_t *buf, int start_row, int start_col,
                        int end_row, int end_col) {
  if (buf->state == READ_ONLY)
    return;
  // See bufferInsertChar()'s comment — kill-region reaching a terminal
  // buffer's rows this way is the same class of bug.
  if (buf->type == TERMINAL_BUFF)
    return;
  if (start_row > end_row || (start_row == end_row && start_col >= end_col))
    return;

  int row = start_row, col = start_col;
  while (row < buf->numrows) {
    if (row == end_row && col >= end_col)
      break; // reached the end of the region

    Row *r = &buf->row[row];
    if (col < r->size) {
      Utf8Char deleted = r->characters[col];
      bufferRowDelChar(buf, r, col);
      undoRecord(buf, UNDO_DELETE_CHAR, row, col, deleted);
      if (row == end_row)
        end_col--;
    } else if (row < end_row && row < buf->numrows - 1) {
      // At end-of-line with more region to go: consume the line break
      // by merging the next row up into this one.
      rowJoinNext(buf, row);
      undoRecord(buf, UNDO_JOIN_LINE, row, col, (Utf8Char){0});
      // The merged-in row's text now lives inside `row`, shifted right
      // by `col`. end_col is still expressed in the *old* row's
      // coordinates, so it has to be rebased into the merged line or
      // the loop terminates immediately and silently leaves the rest
      // of the region undeleted (end_col is typically far smaller than
      // the join column — e.g. deleting "\n    " to reindent a line).
      end_row--;
      if (row == end_row)
        end_col += col;
    } else {
      break; // last row, nothing left to consume
    }
  }
  buf->x = start_col;
  buf->y = start_row;
  if (buf->type != TERMINAL_BUFF)
    buf->dirty++;
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

  if (start_row < end_row || (start_row == end_row && start_col < end_col))
    bufferDeleteRegion(buf, start_row, start_col, end_row, end_col);

  int row = start_row, col = start_col;
  const unsigned char *p = (const unsigned char *)new_text;
  while (p && *p) {
    if (*p == '\n') {
      rowSplitAt(buf, row, col);
      undoRecord(buf, UNDO_SPLIT_LINE, row, col, (Utf8Char){0});
      row++;
      col = 0;
      p++;
    } else {
      Utf8Char ch;
      p += utf8CreateChar(p, &ch);
      bufferRowInsertChar(buf, &buf->row[row], col, &ch);
      undoRecord(buf, UNDO_INSERT_CHAR, row, col, ch);
      col++;
    }
  }
  buf->x = col;
  buf->y = row;
  if (buf->type != TERMINAL_BUFF)
    buf->dirty++;
}

/***********************************************************************************************************************
 *
 *                                                 git integration
 *
 **********************************************************************************************************************/

/**
 * @brief Thin git-flavored wrappers over the shared subprocess helper.
 * @details These were the original implementation; project search needed
 * the same hardened fork/pipe/poll-with-deadline logic, so the body moved
 * to subprocess.c and these stayed behind as the git integration's own
 * defaults — a 1s deadline (this runs synchronously inside bufferCreate()
 * on every file open, so it must never be the reason a file takes a
 * visible moment to appear) and no output cap, since git's own output
 * here is a branch name or one file's diff.
 */
static char *gitRunCommand(const char *cmd) {
  const long GIT_COMMAND_TIMEOUT_MS = 1000;
  return runCommandCapture(cmd, GIT_COMMAND_TIMEOUT_MS, 0);
}

static void gitShellQuote(const char *path, char *out, size_t out_size) {
  shellQuote(path, out, out_size);
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
  free(buf->git_lines);
  buf->git_lines = NULL;
  buf->git_lines_count = 0;
  buf->git_available = 0;
  buf->git_dirty = 0;
  buf->git_branch[0] = '\0';

  if (buf->type != FILE_BUFF || !buf->path || !buf->fullname)
    return;

  char *dir_str = (char *)utf8ToString(buf->path);
  char *file_str = (char *)utf8ToString(buf->fullname);

  char qdir[PATH_MAX + 4], qfile[PATH_MAX + 4];
  gitShellQuote(dir_str, qdir, sizeof(qdir));
  gitShellQuote(file_str, qfile, sizeof(qfile));

  char cmd[3 * PATH_MAX];
  snprintf(cmd, sizeof(cmd),
           "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", qdir);
  char *branch = gitRunCommand(cmd);
  if (!branch) {
    free(dir_str);
    free(file_str);
    return; // not a git repo (or git isn't installed) — leave everything c
            // eared
  }
  strncpy(buf->git_branch, branch, sizeof(buf->git_branch) - 1);
  buf->git_branch[sizeof(buf->git_branch) - 1] = '\0';
  buf->git_available = 1;
  free(branch);

  snprintf(cmd, sizeof(cmd), "git -C %s status --porcelain -- %s 2>/dev/null",
           qdir, qfile);
  char *status = gitRunCommand(cmd);
  buf->git_dirty = status != NULL;
  free(status);

  if (buf->numrows > 0) {
    buf->git_lines = calloc(buf->numrows, sizeof(GitLineStatus));
    buf->git_lines_count = buf->git_lines ? buf->numrows : 0;
  }

  snprintf(cmd, sizeof(cmd), "git -C %s diff --no-color -U0 -- %s 2>/dev/null",
           qdir, qfile);
  char *diff = gitRunCommand(cmd);
  if (diff && buf->git_lines) {
    char *line = strtok(diff, "\n");
    while (line) {
      if (line[0] == '@' && line[1] == '@') {
        int old_start, old_lines, new_start, new_lines;
        if (gitParseHunkHeader(line, &old_start, &old_lines, &new_start,
                               &new_lines)) {
          if (new_lines > 0) {
            GitLineStatus status2 =
                (old_lines == 0) ? GIT_LINE_ADDED : GIT_LINE_MODIFIED;
            for (int i = 0; i < new_lines; i++) {
              int idx = new_start - 1 + i; // 0-based
              if (idx >= 0 && idx < buf->git_lines_count)
                buf->git_lines[idx] = status2;
            }
          } else if (new_start > 0) {
            // Pure deletion: no added/changed line of its own to mark —
            // flag the line right before it as the nearest visible anchor.
            int idx = new_start - 1;
            if (idx >= 0 && idx < buf->git_lines_count)
              buf->git_lines[idx] = GIT_LINE_MODIFIED;
          }
        }
      }
      line = strtok(NULL, "\n");
    }
  }
  free(diff);
  free(dir_str);
  free(file_str);
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
  if (E.buffers == NULL)
    return -1;
  int first_buffer_id = E.buffers->buffer_id;
  Buffer *buffer = E.buffers;
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
  if (E.buffers == NULL || !buf->fullname)
    return -1;
  int first_buffer_id = E.buffers->buffer_id;
  struct buffer_t *b = E.buffers;
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
 * @param buffer_id The buffer ID to find
 * @return Pointer to buffer, NULL if not found
 */
struct buffer_t *bufferFindById(int buffer_id) {
  if (E.buffers == NULL)
    return NULL;
  int first_buffer_id = E.buffers->buffer_id;
  Buffer *buffer = E.buffers;
  do {
    if (buffer->buffer_id == buffer_id) {
      return buffer;
    }
    buffer = buffer->next;
  } while (buffer->buffer_id != first_buffer_id);
  return NULL;
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
  appDebug("Creating new buffer");

  Utf8Char *filename = NULL;
  char *string_filename = NULL;

  if (type == FILE_BUFF) {
    Utf8Char *last_slash = lastSlashUtf8(path);
    const Utf8Char *name_start = last_slash ? last_slash + 1 : path;

    filename = (Utf8Char *)calloc(utf8StringLen(name_start), sizeof(Utf8Char));
    if (!filename)
      return -1;
    utf8Cpy(filename, name_start);
  } else if (type == TERMINAL_BUFF) { // path is already just a plain name, no
      unsigned char temp_filename[5] = "term";
      filename = stringToUtf8(temp_filename);
                                    // splitting needed
    if (!filename)
      return -1;
  } else // EXPLORER_BUFF: `path` IS the buffer's display name directly
         // (e.g. "explorer", "*tree*") — not a real filesystem path, so
         // no slash-splitting either. Distinct names let multiple kinds
         // of scratch buffer (explorer sidebar, tree dump, ...) coexist
         // without bufferFindByFilename() treating them as the same one.
  {
    filename = (Utf8Char *)calloc(utf8StringLen(path), sizeof(Utf8Char));
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
    const int existing_id = bufferFindByFilename(filename);
    if (existing_id != -1) {
      free(filename);
      return bufferSwitch(existing_id);
    }
  }

  if (E.number_of_buffer >= 64) {
    editorSetStatusMessage("Error: maximum buffers reached (64)");
    free(filename);
    return -1;
  }

  struct buffer_t *new_buf = calloc(1, sizeof(struct buffer_t));
  new_buf->buffer_id = buffer_id++;
  new_buf->type = type;
  new_buf->state = state;
  new_buf->x = 0;
  new_buf->y = 0;
  new_buf->dirty = 0;
  new_buf->b_lsp_open = 0;

  new_buf->filename = calloc(utf8StringLen(filename), sizeof(Utf8Char));
  if (!new_buf->filename) {
    free(filename);
    return -1;
  }
  utf8Cpy(new_buf->filename, filename);
  free(filename); // done with the local copy either way

  E.number_of_buffer++;

  if (type == FILE_BUFF) {
    char fullname[PATH_MAX + 1];
    char *path_str = (char *)utf8ToString(path);

    if (realpath(path_str, fullname) == NULL) {
      // File doesn't exist yet, or path invalid — fall back to the raw path
      snprintf(fullname, sizeof(fullname), "%s", path_str);
    }
    free(path_str);

    new_buf->fullname = stringToUtf8((unsigned char *)fullname);
    new_buf->path = stringToUtf8((unsigned char *)dirname(fullname));

    // bufferMaybeStartLsp() checks itself whether a server is
    // registered for this extension — no need to gate the call here too.
    bufferMaybeStartLsp(new_buf);

    appDebug("opening file");
    editorOpen(new_buf);
    appDebug("buffer opened");

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

    char *fullname_str = (char *)utf8ToString(new_buf->fullname);
    Lisp hook_args = lisp_cons(
        lisp_make_string2(fullname_str, E.ctx),
        lisp_cons(lisp_make_int(new_buf->buffer_id), lisp_null(), E.ctx),
        E.ctx);
    runHooks("buffer-opened", hook_args);
    free(fullname_str);
  } else if (type == TERMINAL_BUFF) // no real filesystem path — don't touch r
                                    // alpath/dirname
  {
    new_buf->child_pid = -1;
    new_buf->pty_master_fd = -1;
    new_buf->path = NULL;
    new_buf->fullname = stringToUtf8("term");

    EditorPane *active = splitScreenGetActivePane();
    if (ptySessionSpawn(new_buf, NULL, active->height, active->width) != 0) {
      editorSetStatusMessage("Error: failed to start shell");
      E.number_of_buffer--; // undo the early increment above — this buffer n
                            // ver came up
      return -1;
    }
    struct prefix_t prefix = find_prefix("term");
    E.prefix_state = prefix.prefix_id;
  } else // EXPLORER_BUFF: no real filesystem path, no rows yet — Lisp
         // populates content via editor-scratch-set-lines. Doesn't touch
         // the active pane or prefix state itself; callers that need the
         // sidebar split or "explorer" keybind routing set those up
         // explicitly (see editor-explorer-sidebar-open), since a scratch
         // buffer can also just be opened plainly (e.g. a tree dump).
  {
    new_buf->child_pid = -1;
    new_buf->pty_master_fd = -1;
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
  free(string_filename);
  if (E.buffers != NULL) {
    // Splice new_buf in right after E.buffers. The node that used to
    // be there (old_next) needs its ->prev fixed up too, or walking
    // backward from it forever finds the node that's no longer
    // actually its predecessor — without this, the very first buffer
    // ever created ends up with a ->prev that permanently points to
    // itself instead of wrapping around to the last one.
    struct buffer_t *old_next = E.buffers->next;
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
 * @param buffer_id The buffer ID to switch to
 * @return 0 on success, -1 on failure
 */
int bufferSwitch(int buffer_id) {
  struct buffer_t *target = NULL;
  target = bufferFindById(buffer_id);
  if (target == NULL) {
    editorSetStatusMessage("Error: buffer not found");
    return -1;
  }
  E.layout.panes[E.layout.active_pane].buffer_id = buffer_id;

  // Same focus-prefix routing splitScreenSwitchPane() does, so a buffer
  // that carries its own keymap gets it however you arrive: the buffer
  // switcher and CTRL-x ARROW-RIGHT come through here, not through a
  // pane switch. This also covers re-opening the terminal while its
  // buffer already exists — bufferCreate() dedups to bufferSwitch() and
  // returns before ever reaching the code that sets the "term" prefix.
  const char *prefix_name = NULL;
  if (target->type == TERMINAL_BUFF)
    prefix_name = "term";
  else if (target->type == EXPLORER_BUFF)
    prefix_name = target->focus_prefix[0] ? target->focus_prefix : "explorer";

  if (prefix_name) {
    struct prefix_t prefix = find_prefix(prefix_name);
    if (prefix.prefix_id != -1)
      E.prefix_state = prefix.prefix_id;
  } else {
    E.prefix_state = 0; // plain file buffer: never inherit a stale keymap
  }

  editorSetStatusMessage("Switched to: %u (buffer %d)", target->filename,
                         buffer_id);
  return 0;
}

/**
 * @brief Closes a buffer by ID
 * @details Frees buffer resources and removes from buffer list.
 * Prompts to save unsaved changes. If closing current buffer,
 * switches to next available buffer.
 * @param buffer_id The buffer ID to close
 * @return 0 on success, -1 on failure
 */
int bufferClose(int buffer_id) {
  struct buffer_t *buf = bufferFindById(buffer_id);
  if (buf == NULL) {
    editorSetStatusMessage("Error: buffer not found");
    return -1;
  }

  EditorPane *active = splitScreenGetActivePane();

  // If this is the current buffer, find next buffer to switch to
  if (active->buffer_id == buffer_id) {
    int next_id = -1;
    if (buf->next != NULL) {
      next_id = buf->next->buffer_id;
    }

    if (next_id != -1) {
      bufferSwitch(next_id);
    } else {
      // No other buffers, clear editor
      editorCloseFile();
    }
  }

  // Free buffer resources
  free(buf->filename);
  buf->filename = NULL;
  free(buf->undo_stack.entries);
  free(buf->redo_stack.entries);
  free(buf->git_lines);
  free(buf->comment_state);
  buf->buffer_id = -1;

  buf->next->prev = buf->prev;
  buf->prev->next = buf->next;

  E.number_of_buffer--;

  if (buf->type == TERMINAL_BUFF) {
    ptySessionClose(buf);
  }

  editorSetStatusMessage("Closed buffer %d", buffer_id);
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
  if (!utf8FilenameExtension(filename, ext, sizeof(ext)))
    return NULL;

  for (int i = 0; i < E.number_of_lsp_servers; i++)
    if (strcmp(E.lsp_servers[i].extension, ext) == 0)
      return &E.lsp_servers[i];
  return NULL;
}

/**
 * @brief On the first file opened in a session whose extension has a
 * registered lsp-server, ask the user whether to start it and where the
 * project root is. Only asks once — a decline (or ESC) sets state to
 * LSP_SHUTDOWN so it won't ask again.
 * @note Called before editorOpen() loads the buffer's content — it only
 * needs the filename, not the text. See bufferMaybeSyncLsp() for buffers
 * created *after* the server's already running, which does need the text
 * and so must run after editorOpen().
 */
void bufferMaybeStartLsp(struct buffer_t *buf) {
  if (!E.constantes.LSP)
    return;
  if (E.lsp_client->state != LSP_NOT_STARTED)
    return; // already asked, already running, or shut down

  struct lspServerConfig_t *cfg = lspServerConfigFor(buf->filename);
  if (!cfg)
    return; // no server registered for this extension

  Utf8Char *answer =
      editorPrompt("Start LSP server for this project? (y/n): %u", NULL, 0);
  if (!answer || answer->length != 1 ||
      (answer->character[0] != 'y' && answer->character[0] != 'Y')) {
    free(answer);
    E.lsp_client->state =
        LSP_SHUTDOWN; // declined — don't ask again this session
    return;
  }
  free(answer);

  Utf8Char *default_root = buf->path ? buf->path : buf->fullname;
  Utf8Char *root = editorPrompt("Project root: %u", default_root,
                                1); // bPathMode=1: Tab completes paths
  if (!root) {
    E.lsp_client->state = LSP_SHUTDOWN;
    return;
  }

  char *root_str = (char *)utf8ToString(root);
  free(root);

  strncpy(E.lsp_client->language_id, cfg->language_id,
          sizeof(E.lsp_client->language_id) - 1);
  E.lsp_client->language_id[sizeof(E.lsp_client->language_id) - 1] = '\0';
  strncpy(E.lsp_client->command, cfg->argv[0],
          sizeof(E.lsp_client->command) - 1);
  E.lsp_client->command[sizeof(E.lsp_client->command) - 1] = '\0';

  if (lspStart(E.lsp_client, root_str, cfg->argv) != 0)
    editorSetStatusMessage("Failed to start LSP server");

  free(root_str);
}

int lspServerConfigMatchesRunning(const struct lspServerConfig_t *cfg) {
  if (!cfg || !cfg->argv[0] || !E.lsp_client)
    return 0;
  if (E.lsp_client->state == LSP_NOT_STARTED ||
      E.lsp_client->state == LSP_SHUTDOWN)
    return 0;
  return strcmp(cfg->argv[0], E.lsp_client->command) == 0;
}

/**
 * @brief Syncs a newly-opened buffer with an LSP server that's already
 * running (e.g. a second file opened after the first one started it).
 * @note Must run after editorOpen() — it sends the buffer's actual text,
 * so the rows need to already be loaded. The one-time startup flow in
 * bufferMaybeStartLsp() runs *before* editorOpen() instead, since the
 * retroactive didOpen loop in lsp_on_initialize_response() (which fires
 * once the handshake completes, well after this buffer's own
 * bufferCreate() call has fully returned) covers the very first buffer.
 */
void bufferMaybeSyncLsp(struct buffer_t *buf) {
  if (!E.constantes.LSP || !E.lsp_client)
    return;
  if (E.lsp_client->state != LSP_READY)
    return; // not ready yet — the retroactive didOpen loop will cover it
  if (buf->b_lsp_open)
    return; // already synced

  struct lspServerConfig_t *cfg = lspServerConfigFor(buf->filename);
  if (!cfg)
    return; // no server registered for this extension
  // ...and that server has to be the one actually running — "only one
  // language server runs at a time" means a file whose own registered
  // server is something else (e.g. this is a .c file but a LaTeX
  // buffer opened first and started digestif) must not be forwarded to
  // whatever's running instead. It used to be: any registered extension
  // was enough, so the second file's content went to the wrong server
  // tagged with the wrong languageId.
  if (!lspServerConfigMatchesRunning(cfg))
    return;

  // Same file already open (and synced) in another buffer — the server
  // only expects one didOpen per URI, so just piggyback on that
  // buffer's already-open document instead of sending a second one.
  if (bufferFindOtherLspOpen(buf) != -1) {
    buf->b_lsp_open = 1;
    buf->lsp_synced_dirty = buf->dirty;
    return;
  }

  lspDidOpen(E.lsp_client, buf);
  buf->b_lsp_open = 1;
  buf->lsp_synced_dirty = buf->dirty;
}

/**
 * @brief Lists all open buffers
 * @details Prints buffer information to status message
 * @return Number of open buffers
 */
int bufferListAll(void) {
  if (E.number_of_buffer == 0) {
    editorSetStatusMessage("No buffers open");
    return 0;
  }

  char buf[256] = "";
  int offset = 0;
  EditorPane *active = splitScreenGetActivePane();
  int first_buffer_id = E.buffers->buffer_id;
  struct buffer_t *b = E.buffers;
  do {
    char marker = (b->buffer_id == active->buffer_id) ? '*' : ' ';
#if 0 // TODO: Remake
        offset += utf8Printf(&buf[offset], sizeof(buf) - offset,
                           "%c%d:%s ", marker, b->buffer_id,
                           b->filename ? b->filename : "[No Name]");
        b = b->next;
#endif

  } while (b->buffer_id != first_buffer_id);
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
  for (int i = 0; i < row->size; i++) {
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
  while (1) {
    char *match = strstr(search_from, needle);
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
  appDebug("searching\n");
  Utf8Char *query = editorPrompt("Search: %u (ESC to cancel)", NULL, 0);
  if (!query)
    return;

  char *query_str = (char *)utf8ToString(query);
  free(query);
  if (query_str[0] == '\0') {
    free(query_str);
    return;
  }

  for (int i = buf->y; i < buf->numrows; i++) {
    Row *row = &buf->row[i];
    char *row_str = (char *)utf8ToString(row->characters);

    int start_char = (i == buf->y) ? buf->x + 1 : 0;
    int start_byte = 0;
    for (int c = 0; c < start_char && c < row->size; c++)
      start_byte += row->characters[c].length;
    int row_bytes = (int)strlen(row_str);
    if (start_byte > row_bytes)
      start_byte = row_bytes;

    char *match = strstr(row_str + start_byte, query_str);
    if (match) {
      buf->y = i;
      buf->x = bufferByteOffsetToCharIndex(row, (int)(match - row_str));
      free(row_str);
      break;
    }
    free(row_str);
  }
  free(query_str);
}

/**
 * @brief Searches backward for a string in the document
 * @details Mirrors bufferFind() but scans from the current cursor position
 * back toward the start of the buffer, stopping at the last match before
 * the cursor on the current line. Does not wrap around past the start.
 * @see bufferFind()
 */
void bufferFindReverse(struct buffer_t *buf) {
  appDebug("searching\n");
  Utf8Char *query = editorPrompt("Reverse search: %u (ESC to cancel)", NULL, 0);
  if (!query)
    return;

  char *query_str = (char *)utf8ToString(query);
  free(query);
  if (query_str[0] == '\0') {
    free(query_str);
    return;
  }

  for (int i = buf->y; i >= 0; i--) {
    Row *row = &buf->row[i];
    char *row_str = (char *)utf8ToString(row->characters);

    int end_char = (i == buf->y) ? buf->x : row->size;
    int end_byte = 0;
    for (int c = 0; c < end_char && c < row->size; c++)
      end_byte += row->characters[c].length;

    char *match = bufferFindLastMatchBefore(row_str, query_str, end_byte);
    if (match) {
      buf->y = i;
      buf->x = bufferByteOffsetToCharIndex(row, (int)(match - row_str));
      free(row_str);
      break;
    }
    free(row_str);
  }
  free(query_str);
}

void bufferInsertRow(struct buffer_t *buffer, int at, Utf8Char *string,
                     int len) {
  if (at < 0 || at > buffer->numrows)
    return;
  Row *tmp = recalloc(buffer->row, sizeof(Row) * buffer->numrows,
                      sizeof(Row) * (buffer->numrows + 1));
  if (!tmp)
    return;
  buffer->row = tmp;

  /* Shift existing rows to make room at 'at' — no at++ */
  if (at < buffer->numrows) {
    memmove(&buffer->row[at + 1], &buffer->row[at],
            sizeof(Row) * (buffer->numrows - at)); /* not -at+1 */
  }

  buffer->row[at].size = (int)len;
  buffer->row[at].characters = calloc((len + 1), sizeof(Utf8Char));
  buffer->row[at].attrs = NULL;

  if (!buffer->row[at].characters)
    return;
  memcpy(buffer->row[at].characters, string, len * sizeof(Utf8Char));
  /* always NUL-terminate */
  END_OF_STRING(buffer->row[at].characters[len]);

  buffer->numrows++;
  if (buffer->type != TERMINAL_BUFF) {
    buffer->dirty++;
  }
  syntaxHighlighterInvalidate(buffer);
}

void bufferFreeRow(Row *row) {
  free(row->characters);
  free(row->attrs);
}

/**
 * \fn editorRowInsertChar(erow *row, int at, int c)
 * \param at Index of where we want to insert the char */

void bufferRowInsertChar(struct buffer_t *buffer, Row *row, int at,
                         Utf8Char *character) {
  if (buffer->state == READ_ONLY)
    return;
  if (at < 0 || at > row->size)
    return;

  Utf8Char *new_chars = (Utf8Char *)recalloc(
      row->characters, row->size * sizeof(Utf8Char),
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
  }
  syntaxHighlighterInvalidate(buffer);
}

/**
 * \fn bufferRowDelChar(struct bufferConfig *E, frow *frow, int at)
 * \brief Delete the a char at the chosen position on the given row
 * \param at Index of the char to delete
 * \param row Row on operation is made
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
  syntaxHighlighterInvalidate(buffer);
}

void bufferInsertChar(Utf8Char *character) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
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
    Utf8Char end_string;
    END_OF_STRING(end_string);
    bufferInsertRow(buf, buf->numrows, &end_string, 0);
  }
  int col = buf->x;
  bufferRowInsertChar(buf, &buf->row[buf->y], col, character);
  undoRecord(buf, UNDO_INSERT_CHAR, buf->y, col, *character);
  buf->x = col + 1;
  buf->b_has_changed = 1;
}

void bufferDelChar(void) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);

  // See bufferInsertChar()'s comment — same reasoning, same risk.
  if (buf->type == TERMINAL_BUFF)
    return;

  /* Nothing to delete */
  if (buf->numrows == 0)
    return;
  if (buf->x == 0 && buf->y == 0)
    return;

  /* Use row_offset, not col_offset, for row indexing */
  Row *r = &buf->row[buf->y];

  if (buf->x > 0) {
    int col = buf->x - 1;
    Utf8Char deleted = r->characters[col];
    bufferRowDelChar(buf, r, col);
    undoRecord(buf, UNDO_DELETE_CHAR, buf->y, col, deleted);
    buf->x = col;
  } else {
    /* Merge current row into the previous one */
    int target_row = buf->y - 1;
    int prev_char_count = buf->row[target_row].size;
    rowJoinNext(buf, target_row);
    undoRecord(buf, UNDO_JOIN_LINE, target_row, prev_char_count, (Utf8Char){0});

    active->cursor_x = prev_char_count;
    buf->x = prev_char_count;
    active->cursor_y--;
    buf->y--;
  }
  if (buf->type != TERMINAL_BUFF) {
    buf->dirty++;
  }
}

void bufferInsertNewLine(void) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);

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
    int split_row = buf->y, split_col = buf->x;
    rowSplitAt(buf, split_row, split_col);
    undoRecord(buf, UNDO_SPLIT_LINE, split_row, split_col, (Utf8Char){0});
  }
  buf->y++;
  buf->x = 0;
}

char *bufferToText(struct buffer_t *buf) {
  size_t total = 0;
  for (int i = 0; i < buf->numrows; i++) {
    Row *row = &buf->row[i];
    for (int j = 0; j < row->size; j++)
      total += row->characters[j].length;
    total += 1; // '\n'
  }

  char *text = malloc(total + 1);
  if (!text)
    return NULL;

  char *p = text;
  for (int i = 0; i < buf->numrows; i++) {
    Row *row = &buf->row[i];
    for (int j = 0; j < row->size; j++) {
      memcpy(p, row->characters[j].character, row->characters[j].length);
      p += row->characters[j].length;
    }
    *p++ = '\n';
  }
  *p = '\0';
  return text;
}
