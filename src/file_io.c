/**
 * @file file_io.c
 * @brief File I/O operations module for the Beluga text editor
 * @details Handles file loading, saving, searching, and buffer management.
 * Provides functionality for opening/closing files, persisting changes to disk,
 * and searching for text patterns within the document.
 */

#include "../include/file_io.h"
#include "../include/autosave.h"
#include "../include/syntax_highlighter.h"
#include "../include/galloc.h"
#include "../include/editor_op.h"
#include "../include/input.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/split_screen.h"
#include "../include/completion.h"
#include "../include/os.h"
#include "../include/utils.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/utf8.h"
#include "../include/builtins.h"
#include "../include/project.h"

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
 * @brief Expands raw tab bytes (0x09) in one line into spaces, aligned to
 * real tab stops (every TAB_LENGTH columns), before the line ever reaches
 * a Row.
 * @details Every OTHER way a tab could end up in a buffer already goes
 * through spaces instead — editor-insert-tab (bound to the TAB key)
 * inserts TAB_LENGTH literal space characters, never a raw 0x09 — so a
 * buffer's rows were never actually expected to contain one. Loading a
 * file is the one path that bypassed that: whatever bytes were on disk
 * went straight into the row unchanged, tab included.
 *
 * That's not just a cosmetic gap. Nothing in the renderer's width
 * accounting (gutter width, content_width clipping, the trailing pad-to-
 * width loop) has any idea a raw 0x09 can occupy more than the one column
 * everything else assumes — but a real terminal expands it to the next
 * tab stop, anywhere from 1 to TAB_LENGTH columns depending on where it
 * lands. The gap between what beluga budgeted and what the terminal
 * actually consumed compounds across a row and, once it's enough to
 * exceed the pane's width, corrupts every row below it as the terminal's
 * own auto-wrap kicks in somewhere beluga never intended a line break.
 *
 * Expanding at load time — rather than teaching every column-position
 * consumer (cursor movement, undo, the syntax highlighter, selection
 * bounds) to treat a tab as variable-width — keeps a buffer's rows in the
 * same "1 character = 1 column" world every one of those already assumes,
 * at the cost of the file's own tab bytes not surviving a save unchanged
 * (they come back as the equivalent spaces) — the same trade-off
 * editor-insert-tab already makes for freshly-typed ones.
 * @return A newly malloc'd, tab-expanded copy of `line` if it contained
 * any tabs (caller frees it), or NULL if it didn't (caller keeps using
 * `line` as-is — the common case, worth not allocating for).
 * @param line The line to expand tabs in
 * @param len Length of `line`, in characters
 * @param out_len Out-param: length of the returned copy, if one was made
 */
Utf8Char* expandTabsForLine(const Utf8Char* line, int len, int* out_len)
{
  int has_tab = 0;
  int i;
  int tab_len;
  int col;
  int expanded_len;
  int spaces;
  Utf8Char* out;
  int j;
  Utf8Char space = { .character = { ' ', 0, 0, 0 }, .length = 1 };
  int s;
  for (i = 0; i < len; i++)
    if (line[i].length == 1 && line[i].character[0] == '\t') { has_tab = 1; break; }
  if (!has_tab)
    return NULL;

  tab_len = E.constantes.TAB_LENGTH > 0 ? E.constantes.TAB_LENGTH : 4;

  // Worst case, a tab at column 0 costs tab_len columns; one pass to find
  // the real expanded length, then a second to actually fill it, rather
  // than repeatedly grow a single buffer as we go.
  col = 0; expanded_len = 0;
  for (i = 0; i < len; i++)
  {
    if (line[i].length == 1 && line[i].character[0] == '\t')
    {
      spaces = tab_len - (col % tab_len);
      expanded_len += spaces;
      col += spaces;
    }
    else
    {
      expanded_len++;
      col++;
    }
  }

  out = galloc((size_t) expanded_len * sizeof(Utf8Char));
  if (!out) return NULL;

  j = 0;
  col = 0;
  for (i = 0; i < len; i++)
  {
    if (line[i].length == 1 && line[i].character[0] == '\t')
    {
      spaces = tab_len - (col % tab_len);
      for (s = 0; s < spaces; s++) out[j++] = space;
      col += spaces;
    }
    else
    {
      out[j++] = line[i];
      col++;
    }
  }

  *out_len = expanded_len;
  return out;
}

/**
 * @brief Opens a file for editing
 * @details Loads file content into editor rows, one line per row. If another
 * file is already open, it is closed first (without saving). File is opened in
 * a+ (read/append) mode to allow both reading and modification.
 * @param buffer The buffer to load file_io into — buffer->fullname is the
 * path read from (relative or absolute)
 * @note Updates global editor state E
 * @note If the file doesn't exist, buffer is left empty (new file)
 * @note Newline characters are stripped from loaded lines
 * @see editorInsertRow()
 */
static void editorLoadRows(struct buffer_t* buffer, const Utf8Char* path);
static int askYesNo(const char* question, const struct buffer_t* buffer);

void editorOpen(struct buffer_t* buffer) {
  // The file's project first: its TAB-LENGTH expands tabs.
  projectActivateForBuffer(buffer);
  editorLoadRows(buffer, buffer->fullname);
  fileRememberDiskState(buffer);
}

/**
 * @brief Replaces a buffer's content with the file at `path`
 * @details For content that isn't the buffer's own file (an autosave copy).
 * Rows, undo/redo history and the cursor are reset; the buffer's name and
 * path are left alone. dirty ends up 0, as after editorOpen().
 */
void editorReloadFrom(struct buffer_t* buffer, const Utf8Char* path) {
  int i;
  for (i = 0; i < buffer->numrows; i++)
    bufferFreeRow(&buffer->row[i]);
  gfree(buffer->row);
  buffer->row = NULL;
  buffer->numrows = 0;
  buffer->x = 0;
  buffer->y = 0;
  // The entries point at rows and columns of the old content.
  bufferClearUndo(buffer);
  editorLoadRows(buffer, path);
  syntaxHighlighterInvalidateAfter(buffer, -1);
}

/**
 * @brief Appends the lines of the file at `path` to `buffer`'s rows
 */
static void editorLoadRows(struct buffer_t* buffer, const Utf8Char* path) {
  Utf8Char* file_content;
  Utf8Char* pFileContent;
  Utf8Char* pEndLine;
  int line_length;
  int expanded_len;
  Utf8Char* expanded;
  int first_line;
  int ends_with_cr;
  file_content = utf8ReadFile(path);
  buffer->crlf = 0;
  if (file_content == NULL)
  {
    // File doesn't exist yet: start with an empty, editable buffer, same
    // as vim/emacs. The file itself is created on disk the first time the
    // buffer is saved.
    buffer->dirty = 0;
    return;
  }

  pFileContent = file_content;
  pEndLine = file_content;
  first_line = 1;
  while (*pFileContent->character != '\0')
  {
    while (*pEndLine->character != '\n' && *pEndLine->character != '\0')
    {
      // appDebug("%s %s", file_content->character, pEndLine->character);
      pEndLine++;
    }
    line_length = (int) (pEndLine - pFileContent);

    // CRLF file (Windows, or a Git for Windows checkout): the first line
    // decides, and every "\r\n" then loses its '\r' — kept, it showed up
    // at the end of every line and was saved back doubled.
    ends_with_cr = line_length > 0 && *pEndLine->character == '\n' &&
                   *pFileContent[line_length - 1].character == '\r';
    if (first_line)
      buffer->crlf = ends_with_cr;
    first_line = 0;
    if (buffer->crlf && ends_with_cr)
      line_length--;

    expanded_len = 0;
    expanded = expandTabsForLine(pFileContent, line_length, &expanded_len);
    if (expanded)
    {
      bufferInsertRow(buffer, buffer->numrows, expanded, expanded_len);
      gfree(expanded);
    }
    else
    {
      bufferInsertRow(buffer, buffer->numrows, pFileContent, line_length);
    }

    // Last line without a trailing newline: pEndLine is on the terminator
    // itself, and stepping past it read beyond the array.
    if (*pEndLine->character == '\0')
      break;
    pEndLine++;
    pFileContent = pEndLine;
  }
  gfree(file_content);

  buffer->dirty = 0;
}

/**
 * @brief Writes every row of `buffer` to `fd`, then flushes it to disk and
 * closes it.
 * @return 1, or 0 with the reason in `err`. `fd` is closed either way.
 */
static int writeRows(const struct buffer_t* buffer, FILE* fd, char* err,
                     size_t err_size) {
  const char* eol = buffer->crlf ? "\r\n" : "\n";
  const size_t eol_len = strlen(eol);
  char* line;
  size_t len;
  int ok = 1;
  int i;

  for (i = 0; ok && i < buffer->numrows; ++i) {
    line = (char*) utf8ToString(buffer->row[i].characters);
    if (!line) {
      errno = ENOMEM;
      ok = 0;
      break;
    }
    len = strlen(line);
    ok = fwrite(line, 1, len, fd) == len && fwrite(eol, 1, eol_len, fd) == eol_len;
    gfree(line);
  }
  if (ok && os_file_flush(fd) != 0)
    ok = 0;
  if (!ok)
    snprintf(err, err_size, "%s", strerror(errno));
  if (fclose(fd) != 0 && ok) {
    snprintf(err, err_size, "%s", strerror(errno));
    ok = 0;
  }
  return ok;
}

/**
 * @brief Saves the current file to disk
 * @details Prompts for filename if not set, converts all rows to a buffer,
 * writes to disk using open/ftruncate/write, and updates dirty flag.
 * Displays status messages on success or failure.
 * @note Updates global editor state E (dirty flag)
 * @note If no filename is set, prompts user via editorPrompt()
 * @note Uses O_RDWR | O_CREAT with mode 0644
 * @see editorRowsToString()
 */
void editorSave() {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  FILE *fd;
  char *string_path = NULL;
  char *tmp_path = NULL;
  char err[256];
  int in_place;
  int ok;
  int buffer_id;
  Utf8Char *answer;
  char *answer_str;

  // EXPLORER_BUFF has no real fullname (NULL) — it's generated content,
  // not a file, so there's nothing sensible to write. A terminal's
  // fullname is the placeholder "term", not a file either.
  if (buffer->type != FILE_BUFF) {
    editorSetStatusMessage("Not a file — nothing to save");
    return;
  }

  if (buffer->filename == NULL || buffer->fullname == NULL) {
    // NULL, not "": the parameter is a Utf8Char* (a 5-byte struct array),
    // so a 1-byte "" literal makes editorPrompt's "%u" read past the end of
    // the object. NULL is how it already spells "no placeholder".
    answer = editorPrompt("Save as: %u (ESC to cancel)", NULL, 1);
    answer_str = answer ? (char *)utf8ToString(answer) : NULL;
    gfree(answer);
    if (answer_str == NULL || answer_str[0] == '\0') {
      gfree(answer_str);
      editorSetStatusMessage("Save aborted");
      return;
    }
    // The write goes to fullname: set it (and the directory) from the
    // answer, not just the name shown on the status bar.
    if (bufferSetPath(buffer, answer_str) != 0) {
      gfree(answer_str);
      editorSetStatusMessage("Can't save: out of memory");
      return;
    }
    gfree(answer_str);
    // A new target: nothing on disk to compare against yet, except a file
    // that is already there, which Save-as must not silently replace.
    fileRememberDiskState(buffer);
    if (buffer->disk_known &&
        !askYesNo("%s already exists. Overwrite it? (y/n): %%u", buffer)) {
      editorSetStatusMessage("Save cancelled");
      return;
    }
    buffer->disk_known = 0;
  }
  if ((fileChangedOnDisk(buffer) || buffer->disk_conflict) &&
      !askYesNo("%s changed on disk since it was opened. Overwrite it? (y/n): %%u",
                buffer)) {
    editorSetStatusMessage("Save cancelled");
    return;
  }

  string_path = (char *)utf8ToString(buffer->fullname);

  runHooks("before-save", lisp_cons(lisp_make_string2(string_path, E.ctx), lisp_null(), E.ctx));

  // Written next to the file, then renamed over it (os_replace_file()): a
  // crash or a full disk halfway through used to leave a truncated file.
  tmp_path = utilFormat("%s.beluga-save", string_path);
  if (!tmp_path) {
    editorSetStatusMessage("Can't save: out of memory");
    gfree(string_path);
    return;
  }

  // A directory the user can't create files in (but whose file is
  // writable) can't take the temporary file: write in place there, as
  // before, still checking every write.
  in_place = 0;
  fd = fopen(tmp_path, "wb");
  if (fd == NULL) {
    in_place = 1;
    fd = fopen(string_path, "wb");
  }
  if (fd == NULL) {
    editorSetStatusMessage("Can't save %s: %s", string_path, strerror(errno));
    gfree(tmp_path);
    gfree(string_path);
    return;
  }

  ok = writeRows(buffer, fd, err, sizeof(err));
  if (ok && !in_place &&
      os_replace_file(tmp_path, string_path, err, sizeof(err)) != 0)
    ok = 0;

  if (!ok) {
    // Through the temporary file, the original is untouched. Either way
    // the buffer stays modified.
    if (!in_place)
      remove(tmp_path);
    editorSetStatusMessage("Can't save %s: %s", string_path, err);
    gfree(tmp_path);
    gfree(string_path);
    return;
  }

  buffer->dirty = 0;
  autosaveDiscard(buffer);
  fileRememberDiskState(buffer);
  buffer_id = buffer->buffer_id;
  // Before the hooks, so a message one of them leaves wins.
  editorSetStatusMessage("File saved");
  runHooks("after-save", lisp_cons(lisp_make_string2(string_path, E.ctx), lisp_null(), E.ctx));
  // A hook may have killed the buffer (git's commit-message buffer does).
  buffer = bufferFindById(buffer_id);
  if (buffer)
    bufferRefreshGitInfo(buffer);
  gfree(tmp_path);
  gfree(string_path);
}

/***********************************************************************************************************************
 *
 *                                                 Files changed on disk
 *
 **********************************************************************************************************************/

// The check runs on the run loop's ~50ms tick; files are looked at at most
// once a second.
static time_t s_last_disk_check = 0;

void fileRememberDiskState(struct buffer_t* buffer) {
  char* path;
  OsFileInfo info;
  buffer->disk_known = 0;
  buffer->disk_conflict = 0;
  if (buffer->type != FILE_BUFF || !buffer->fullname)
    return;
  path = (char*) utf8ToString(buffer->fullname);
  if (path && os_file_info(path, &info) == 0 && !info.is_dir) {
    buffer->disk_known = 1;
    buffer->disk_mtime = info.mtime;
    buffer->disk_size = info.size;
  }
  gfree(path);
}

int fileChangedOnDisk(struct buffer_t* buffer) {
  char* path;
  OsFileInfo info;
  int changed;
  // A file that didn't exist when opened (a new file) has nothing to
  // compare against; one deleted since is left alone too: saving
  // recreates it.
  if (buffer->type != FILE_BUFF || !buffer->disk_known || !buffer->fullname)
    return 0;
  path = (char*) utf8ToString(buffer->fullname);
  changed = path && os_file_info(path, &info) == 0 && !info.is_dir &&
            (info.mtime != buffer->disk_mtime || info.size != buffer->disk_size);
  gfree(path);
  return changed;
}

/**
 * @brief editorPrompt() for a y/n question naming `buffer`.
 * @param question Format with one %s (the file name) and the prompt's %%u.
 */
static int askYesNo(const char* question, const struct buffer_t* buffer) {
  char prompt[512];
  char* name;
  char* c;
  Utf8Char* answer;
  int yes;
  // editorPrompt() treats the prompt as a format: keep the name's own %
  // out of it.
  name = (char*) utf8ToString(buffer->filename);
  for (c = name; c && *c; c++)
    if (*c == '%')
      *c = '?';
  snprintf(prompt, sizeof(prompt), question, name ? name : "This file");
  gfree(name);
  // Asked from inside CTRL-x CTRL-s: with the prefix still pending, the
  // which-key panel would cover the question.
  E.prefix_state = 0;
  answer = editorPrompt(prompt, NULL, 0);
  yes = answer && answer->length == 1 &&
        (answer->character[0] == 'y' || answer->character[0] == 'Y');
  gfree(answer);
  return yes;
}

void editorReload(struct buffer_t* buffer) {
  int i;
  const int y = buffer->y;
  const int x = buffer->x;
  for (i = 0; i < buffer->numrows; i++)
    bufferFreeRow(&buffer->row[i]);
  gfree(buffer->row);
  buffer->row = NULL;
  buffer->numrows = 0;
  // The entries point at rows and columns of the old content.
  bufferClearUndo(buffer);
  editorLoadRows(buffer, buffer->fullname);
  fileRememberDiskState(buffer);
  syntaxHighlighterInvalidateAfter(buffer, -1);

  // Same place, as far as the new content allows.
  buffer->y = y < buffer->numrows ? y : (buffer->numrows > 0 ? buffer->numrows - 1 : 0);
  buffer->x = 0;
  if (buffer->numrows > 0)
    buffer->x = x < buffer->row[buffer->y].size ? x : buffer->row[buffer->y].size;

  // A new document version for the server, though no edit made it.
  buffer->edit_seq++;
  if (buffer->b_lsp_open && lspClientForBuffer(buffer)) {
    lspDidChange(lspClientForBuffer(buffer), buffer);
    buffer->lsp_synced_seq = buffer->edit_seq;
  }
  bufferRefreshGitInfo(buffer);
}

void fileCheckDiskChanges(void) {
  const time_t now = time(NULL);
  EditorPane* pane;
  struct buffer_t* buf;
  int i;
  int j;
  int seen;

  if (now == s_last_disk_check || E.prompt_input != NULL)
    return;
  s_last_disk_check = now;

  // Only what's on screen: a hidden buffer is checked when it's shown.
  for (i = 0; i < E.layout.num_panes; i++) {
    pane = &E.layout.panes[i];
    seen = 0;
    for (j = 0; j < i; j++)
      if (E.layout.panes[j].buffer_id == pane->buffer_id)
        seen = 1;
    buf = seen ? NULL : bufferFindById(pane->buffer_id);
    if (!buf || !fileChangedOnDisk(buf))
      continue;

    if (!buf->dirty) {
      editorReload(buf);
      editorSetStatusMessage("Reloaded (changed on disk)");
    } else if (askYesNo("%s changed on disk. Reload it and lose your changes? (y/n): %%u",
                        buf)) {
      editorReload(buf);
      editorSetStatusMessage("Reloaded (changed on disk)");
    } else {
      // Keep the user's version; ask again only if it changes again. A save
      // will still ask before overwriting.
      fileRememberDiskState(buf);
      buf->disk_conflict = 1;
      editorSetStatusMessage("Kept your version; saving will ask before overwriting");
    }
  }
}
