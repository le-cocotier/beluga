/**
 * @file file_io.c
 * @brief File I/O operations module for the Beluga text editor
 * @details Handles file loading, saving, searching, and buffer management.
 * Provides functionality for opening/closing files, persisting changes to disk,
 * and searching for text patterns within the document.
 */

#include "../include/file_io.h"
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
  active->cursor_x = 0;
  active->cursor_y = 0;
  active->x_offset = 0;
  active->y_offset = 0;
  for (int i = 0; i < buf->numrows; ++i) {
    free(buf->row[i].characters);
  }
  buf->numrows = 0;
  free(buf->row);
  buf->row = NULL;
  buf->dirty = 0;
  free(buf->filename);
  buf->filename = NULL;
  END_OF_STRING(E.status_msg[0]);
  E.status_msg_time = 0;
}

/**
 * @brief Opens a file for editing
 * @details Loads file content into editor rows, one line per row. If another
 * file is already open, it is closed first (without saving). File is opened in
 * a+ (read/append) mode to allow both reading and modification.
 * @param buffer Path to the file to open (relative or absolute)
 * @note Updates global editor state E
 * @note If the file doesn't exist, buffer is left empty (new file)
 * @note Newline characters are stripped from loaded lines
 * @see editorInsertRow()
 */
void editorOpen(struct buffer_t* buffer) {


  Utf8Char* file_content = utf8ReadFile(buffer->fullname);
  if (file_content == NULL)
  {
    // File doesn't exist yet: start with an empty, editable buffer, same
    // as vim/emacs. The file itself is created on disk the first time the
    // buffer is saved.
    buffer->dirty = 0;
    return;
  }

  Utf8Char* pFileContent = file_content;
  Utf8Char* pEndLine = file_content;
  int line_length = 0;
  while (*pFileContent->character != '\0')
  {
    while (*pEndLine->character != '\n')
    {
      // appDebug("%s %s", file_content->character, pEndLine->character);
      pEndLine++;
    }
    line_length = (int) (pEndLine - pFileContent);
    bufferInsertRow(buffer, buffer->numrows, pFileContent, line_length);
    pEndLine++;
    pFileContent = pEndLine;
  }
  free(file_content);

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
    //TODO: utf8
  EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buffer = bufferFindById(active->buffer_id);
  char* string_line;
  FILE *fd;
  char *string_path = NULL;

  // EXPLORER_BUFF has no real fullname (NULL) — it's generated content,
  // not a file, so there's nothing sensible to write.
  if (buffer->type == EXPLORER_BUFF) {
    editorSetStatusMessage("Not a file — nothing to save");
    return;
  }

  if (buffer->filename == NULL) {
    buffer->filename = editorPrompt("Save as: %s (ESC to cancel)", "", 1);
    if (buffer->filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }
  string_path = (char *)utf8ToString(buffer->fullname);

  runHooks("before-save", lisp_cons(lisp_make_string2(string_path, E.ctx), lisp_null(), E.ctx));

  fd = fopen(string_path, "w");
  if (fd != NULL) {
    for (int i = 0; i < buffer->numrows; ++i)
    {
      string_line = (char *)utf8ToString(buffer->row[i].characters);
      if (fwrite(string_line, 1, strlen(string_line), fd) != strlen(string_line)) {
        fclose(fd);
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
        free(string_line);
        free(string_path);
        return;
      }
      free(string_line);
      string_line = NULL;
      fwrite("\n", 1, 1, fd);
    }
    buffer->dirty = 0;
    fclose(fd);
    runHooks("after-save", lisp_cons(lisp_make_string2(string_path, E.ctx), lisp_null(), E.ctx));
    free(string_path);
  }
  editorSetStatusMessage("File saved");
}
