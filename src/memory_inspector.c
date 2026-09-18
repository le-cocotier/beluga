/**
 * @file memory_inspector.c
 * @brief Debug-only buffer that lists every allocation currently tracked
 * by gutils (galloc/grealloc), for spotting leaks while the editor runs.
 * @details Same shape as project_search.c's results buffer: an ordinary
 * read-only scratch buffer (EXPLORER_BUFF) named "*memory*", singleton by
 * buffer id, re-rendered in place on refresh rather than recreated.
 */

#include "../include/memory_inspector.h"

#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"

#include <stdio.h>

extern EditorConfig E;

#define MEMORY_BUFFER_NAME "*memory*"
#define MEMORY_PREFIX_NAME "memory"

static int s_memory_buffer_id = -1;

static void clearBufferRows(Buffer *buf) {
  for (int i = 0; i < buf->numrows; i++) {
    gfree(buf->row[i].characters);
    gfree(buf->row[i].attrs);
  }
  gfree(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
  buf->x = 0;
  buf->y = 0;
}

static void addRow(Buffer *buf, const char *text) {
  Utf8Char *u = stringToUtf8((unsigned char *)text);
  if (!u)
    return;
  bufferInsertRow(buf, buf->numrows, u, utf8StringLen(u) - 1);
  gfree(u);
}

static Buffer *openMemoryBuffer(void) {
  Buffer *buf = bufferFindById(s_memory_buffer_id);
  if (!buf || buf->type != EXPLORER_BUFF) {
    Utf8Char *name = stringToUtf8((unsigned char *)MEMORY_BUFFER_NAME);
    if (!name)
      return NULL;
    s_memory_buffer_id = bufferCreate(name, READ_ONLY, EXPLORER_BUFF);
    gfree(name);
    buf = bufferFindById(s_memory_buffer_id);
    if (!buf)
      return NULL;
    snprintf(buf->focus_prefix, sizeof(buf->focus_prefix), "%s",
             MEMORY_PREFIX_NAME);
  }
  return buf;
}

static void renderSnapshot(Buffer *buf) {
  clearBufferRows(buf);

  int count = gBlockCount();
  size_t total = 0;
  for (struct dataBlock_t *p = gBlocks(); p; p = p->lastBlock)
    total += p->size;

  char header[128];
  snprintf(header, sizeof(header), "%d tracked block%s, %zu bytes total",
           count, count == 1 ? "" : "s", total);
  addRow(buf, header);
  addRow(buf, "press r to refresh");
  addRow(buf, "");

  for (struct dataBlock_t *p = gBlocks(); p; p = p->lastBlock) {
    char row[64];
    snprintf(row, sizeof(row), "%-14p %10zu bytes", p->address, p->size);
    addRow(buf, row);
  }

  buf->dirty = 0;
}

void memoryInspectorOpen(void) {
  Buffer *buf = openMemoryBuffer();
  if (!buf)
    return;
  renderSnapshot(buf);

  EditorPane *active = splitScreenGetActivePane();
  active->buffer_id = s_memory_buffer_id;
  active->cursor_y = buf->y;
  active->cursor_x = 0;
  active->y_offset = 0;
  active->x_offset = 0;

  struct prefix_t prefix = find_prefix(MEMORY_PREFIX_NAME);
  if (prefix.prefix_id != -1)
    E.prefix_state = prefix.prefix_id;

  editorSetStatusMessage("%d tracked block%s — r to refresh, CTRL-x to leave",
                          gBlockCount(), gBlockCount() == 1 ? "" : "s");
}

void memoryInspectorRefresh(void) {
  Buffer *buf = bufferFindById(s_memory_buffer_id);
  if (!buf || buf->type != EXPLORER_BUFF)
    return;
  renderSnapshot(buf);
  editorSetStatusMessage("%d tracked block%s", gBlockCount(),
                          gBlockCount() == 1 ? "" : "s");
}
