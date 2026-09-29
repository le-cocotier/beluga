/**
 * @file file_io.c
 * @brief File I/O operations module for the Beluga text editor
 * @details Handles file loading, saving, searching, and buffer management.
 * Provides functionality for opening/closing files, persisting changes to disk,
 * and searching for text patterns within the document.
 */

#include "../include/file_io.h"
#include "../include/galloc.h"
#include "../include/editor_op.h"
#include "../include/input.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/split_screen.h"
#include "../include/row_op.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "../include/utf8.h"
#include "../include/builtins.h"

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
 * @brief Closes the current file and resets editor state
 * @details Clears all rows, resets cursor position, scroll offsets, and file
 * metadata. Does not prompt to save unsaved changes.
 * @note Updates global editor state E
 */
void editorCloseFile(void) {
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  int i;
  active->cursor_x = 0;
  active->cursor_y = 0;
  active->x_offset = 0;
  active->y_offset = 0;
  for (i = 0; i < buf->numrows; ++i) {
    gfree(buf->row[i].characters);
  }
  buf->numrows = 0;
  gfree(buf->row);
  buf->row = NULL;
  buf->dirty = 0;
  gfree(buf->filename);
  buf->filename = NULL;
  END_OF_STRING(E.status_msg[0]);
  E.status_msg_time = 0;
}

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
void editorOpen(struct buffer_t* buffer) {


  Utf8Char* file_content = utf8ReadFile(buffer->fullname);
  Utf8Char* pFileContent;
  Utf8Char* pEndLine;
  int line_length;
  int expanded_len;
  Utf8Char* expanded;
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
  while (*pFileContent->character != '\0')
  {
    while (*pEndLine->character != '\n' && *pEndLine->character != '\0')
    {
      // appDebug("%s %s", file_content->character, pEndLine->character);
      pEndLine++;
    }
    line_length = (int) (pEndLine - pFileContent);

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
  // TODO(TODO.md): failed-open reported as saved, non-atomic write
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  char* string_line;
  FILE *fd;
  char *string_path = NULL;
  int i;

  // EXPLORER_BUFF has no real fullname (NULL) — it's generated content,
  // not a file, so there's nothing sensible to write.
  if (buffer->type == EXPLORER_BUFF) {
    editorSetStatusMessage("Not a file — nothing to save");
    return;
  }

  if (buffer->filename == NULL) {
    // NULL, not "": the parameter is a Utf8Char* (a 5-byte struct array),
    // so a 1-byte "" literal makes editorPrompt's "%u" read past the end of
    // the object. NULL is how it already spells "no placeholder".
    buffer->filename = editorPrompt("Save as: %u (ESC to cancel)", NULL, 1);
    if (buffer->filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }
  string_path = (char *)utf8ToString(buffer->fullname);

  runHooks("before-save", lisp_cons(lisp_make_string2(string_path, E.ctx), lisp_null(), E.ctx));

  fd = fopen(string_path, "w");
  if (fd != NULL) {
    for (i = 0; i < buffer->numrows; ++i)
    {
      string_line = (char *)utf8ToString(buffer->row[i].characters);
      if (fwrite(string_line, 1, strlen(string_line), fd) != strlen(string_line)) {
        fclose(fd);
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
        gfree(string_line);
        gfree(string_path);
        return;
      }
      gfree(string_line);
      string_line = NULL;
      fwrite("\n", 1, 1, fd);
    }
    buffer->dirty = 0;
    fclose(fd);
    runHooks("after-save", lisp_cons(lisp_make_string2(string_path, E.ctx), lisp_null(), E.ctx));
    bufferRefreshGitInfo(buffer);
    gfree(string_path);
  }
  editorSetStatusMessage("File saved");
}
