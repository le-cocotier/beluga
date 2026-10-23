/**
 * @file debug_ui.c
 * @brief The debugger's marks in the editor's gutter
 */

#include "../include/debug_ui.h"

#include "../include/append_buffer.h"
#include "../include/breakpoints.h"
#include "../include/galloc.h"

#include <string.h>

extern EditorConfig E;

char *debugUiGutterPath(const struct buffer_t *buf) {
  char *path = breakpointsBufferPath(buf);
  if (path && breakpointsInFile(path) == 0) {
    gfree(path);
    return NULL;
  }
  return path;
}

int debugUiWantsGutter(const struct buffer_t *buf) {
  char *path = debugUiGutterPath(buf);
  gfree(path);
  return path != NULL;
}

int debugUiDrawGutter(struct abuf *ab, const char *path, const int line) {
  const Breakpoint *bp;
  const char *glyph;
  if (!path)
    return 0;
  bp = breakpointAt(path, line);
  if (!bp)
    return 0;
  // ● a breakpoint, ◆ one with a condition.
  glyph = bp->condition ? "◆" : "●";
  abAppend(ab, "\x1b[1;31m", 7);
  abAppend(ab, glyph, (int)strlen(glyph));
  abAppend(ab, "\x1b[0m", 4);
  abAppend(ab, E.theme.BACKGROUND_COLOR, (int)strlen(E.theme.BACKGROUND_COLOR));
  abAppend(ab, " ", 1);
  return 1;
}
