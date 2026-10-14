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

// Files changed on disk by another program. editorOpen() and a save record
// the file's mtime and size; fileCheckDiskChanges(), called from the run
// loop, compares them for the buffers on screen (once a second at most): an
// unmodified buffer is reloaded, a modified one asks first, and a save over
// a newer file asks too.
void fileRememberDiskState(struct buffer_t *buffer);
int fileChangedOnDisk(struct buffer_t *buffer);
void fileCheckDiskChanges(void);
// Re-reads the buffer's file, keeping the cursor where it can. Clears
// undo/redo (its entries describe the old content).
void editorReload(struct buffer_t *buffer);
// Copy of `line` with each tab expanded to spaces up to the next TAB-LENGTH
// stop, or NULL if it has no tabs (use `line` as is). Rows never hold a raw
// tab: the renderer counts one cell per character, and a tab written to
// the terminal would jump the cursor and misalign everything after it.
Utf8Char* expandTabsForLine(const Utf8Char* line, int len, int* out_len);

#endif // FILE_IO_H_
