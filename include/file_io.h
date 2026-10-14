#ifndef FILE_IO_H_
#define FILE_IO_H_

#include "data.h"
#include <stdio.h>
#include <stdlib.h>

void editorCloseFile(void);

void editorOpen(struct buffer_t *buffer);

// Replaces the buffer's content with the file at `path` (not its own file:
// an autosave copy). Clears undo/redo; dirty ends up 0.
void editorReloadFrom(struct buffer_t *buffer, const Utf8Char *path);

void editorSave();
// Copy of `line` with each tab expanded to spaces up to the next TAB-LENGTH
// stop, or NULL if it has no tabs (use `line` as is). Rows never hold a raw
// tab: the renderer counts one cell per character, and a tab written to
// the terminal would jump the cursor and misalign everything after it.
Utf8Char* expandTabsForLine(const Utf8Char* line, int len, int* out_len);

#endif // FILE_IO_H_
