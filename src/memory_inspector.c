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
#include "../include/input.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

#define MEMORY_BUFFER_NAME "*memory*"
#define MEMORY_PREFIX_NAME "memory"

static int s_memory_buffer_id = -1;

typedef enum {
  SORT_RECENCY,    // newest allocation first (gutils' own list order)
  SORT_SIZE_DESC,  // biggest block first — the usual "what's eating memory" view
  SORT_LOCATION,   // grouped by file:line, then func
  SORT_MODE_COUNT
} SortMode;

static SortMode s_sort_mode = SORT_RECENCY;

static const char *sortModeLabel(SortMode mode) {
  switch (mode) {
  case SORT_SIZE_DESC: return "size";
  case SORT_LOCATION:  return "location";
  default:             return "recency";
  }
}

static int compareSizeDesc(const void *a, const void *b) {
  const struct dataBlock_t *ba = *(const struct dataBlock_t * const *)a;
  const struct dataBlock_t *bb = *(const struct dataBlock_t * const *)b;
  if (ba->size != bb->size)
    return ba->size > bb->size ? -1 : 1;
  return 0;
}

static int compareLocation(const void *a, const void *b) {
  const struct dataBlock_t *ba = *(const struct dataBlock_t * const *)a;
  const struct dataBlock_t *bb = *(const struct dataBlock_t * const *)b;
  int c = strcmp(ba->file ? ba->file : "", bb->file ? bb->file : "");
  if (c != 0)
    return c;
  if (ba->line != bb->line)
    return ba->line - bb->line;
  return strcmp(ba->func ? ba->func : "", bb->func ? bb->func : "");
}

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

// Snapshots gBlocks() into a plain array so it can be sorted for display
// without touching the real list — galloc/gfree's own bookkeeping (see
// gfree's "is this the last allocated block" check) depends on that list's
// actual insertion order, so it must never be reordered in place.
static struct dataBlock_t **collectSortedBlocks(int *out_count) {
  int count = gBlockCount();
  if (count <= 0) {
    *out_count = 0;
    return NULL;
  }

  struct dataBlock_t **arr = galloc(sizeof(struct dataBlock_t *) * (size_t) count);
  if (!arr) {
    *out_count = 0;
    return NULL;
  }

  int i = 0;
  for (struct dataBlock_t *p = gBlocks(); p && i < count; p = p->lastBlock)
    arr[i++] = p;

  switch (s_sort_mode) {
  case SORT_SIZE_DESC:
    qsort(arr, (size_t) i, sizeof(*arr), compareSizeDesc);
    break;
  case SORT_LOCATION:
    qsort(arr, (size_t) i, sizeof(*arr), compareLocation);
    break;
  default:
    break; // SORT_RECENCY: gBlocks() order is already newest-first
  }

  *out_count = i;
  return arr;
}

static void formatRow(char *out, size_t out_size, const struct dataBlock_t *p) {
  snprintf(out, out_size, "%-14p %8zu bytes  %s:%d (%s)", p->address, p->size,
           p->file ? p->file : "?", p->line, p->func ? p->func : "?");
}

static void renderSnapshot(Buffer *buf) {
  clearBufferRows(buf);

  int count;
  struct dataBlock_t **sorted = collectSortedBlocks(&count);

  size_t total = 0;
  for (int i = 0; i < count; i++)
    total += sorted[i]->size;

  char header[160];
  snprintf(header, sizeof(header),
           "%d tracked block%s, %zu bytes total — sorted by %s", count,
           count == 1 ? "" : "s", total, sortModeLabel(s_sort_mode));
  addRow(buf, header);
  addRow(buf, "r refresh, s cycle sort, w export, CTRL-x leave");
  addRow(buf, "");

  for (int i = 0; i < count; i++) {
    char row[160];
    formatRow(row, sizeof(row), sorted[i]);
    addRow(buf, row);
  }

  gfree(sorted);
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

  // The block count (and so buf->numrows) can shrink between refreshes.
  // Without this, the active pane's cursor/scroll from before the refresh
  // can point past the new row count and read out of bounds — unlike
  // memoryInspectorOpen(), which starts these at 0 already.
  EditorPane *active = splitScreenGetActivePane();
  if (active->buffer_id == s_memory_buffer_id) {
    active->cursor_y = buf->y;
    active->cursor_x = 0;
    active->y_offset = 0;
    active->x_offset = 0;
  }

  editorSetStatusMessage("%d tracked block%s", gBlockCount(),
                          gBlockCount() == 1 ? "" : "s");
}

void memoryInspectorCycleSort(void) {
  s_sort_mode = (SortMode)((s_sort_mode + 1) % SORT_MODE_COUNT);
  memoryInspectorRefresh();
}

void memoryInspectorExport(void) {
  Utf8Char *input =
      editorPrompt("Export memory snapshot to: %u (ESC to cancel)", NULL, 0);
  if (!input)
    return;

  char *path = (char *)utf8ToString(input);
  gfree(input);
  if (!path)
    return;
  if (!*path) {
    gfree(path);
    return;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    editorSetStatusMessage("could not open %s for writing", path);
    gfree(path);
    return;
  }

  int count;
  struct dataBlock_t **sorted = collectSortedBlocks(&count);
  size_t total = 0;
  for (int i = 0; i < count; i++)
    total += sorted[i]->size;

  fprintf(f, "%d tracked block%s, %zu bytes total, sorted by %s\n\n", count,
          count == 1 ? "" : "s", total, sortModeLabel(s_sort_mode));
  for (int i = 0; i < count; i++) {
    char row[160];
    formatRow(row, sizeof(row), sorted[i]);
    fprintf(f, "%s\n", row);
  }
  gfree(sorted);
  fclose(f);

  editorSetStatusMessage("wrote %d block%s to %s", count,
                          count == 1 ? "" : "s", path);
  gfree(path);
}
