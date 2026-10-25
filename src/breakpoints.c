/**
 * @file breakpoints.c
 * @brief The editor's breakpoints: one list keyed by file path, kept in
 * step with the edits made to the files' buffers
 * @details The debugger sends them to the debug adapter when a session
 * starts, and again whenever a file's set changes (the listener); the
 * gutter draws them.
 */

#include "../include/breakpoints.h"

#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static Breakpoint *s_breakpoints;
static int s_count;
static int s_cap;
static void (*s_listener)(const char *path);
static char *s_store; // the file they are kept in across runs, or NULL
static int s_loading; // breakpointsLoad() adding: nothing to tell or save

static void notifyChanged(const char *path) {
  if (s_loading)
    return;
  if (s_store)
    breakpointsSave(s_store);
  if (s_listener)
    s_listener(path);
}

void breakpointsSetListener(void (*listener)(const char *path)) {
  s_listener = listener;
}

static int compareBreakpoints(const void *a, const void *b) {
  const Breakpoint *x = a;
  const Breakpoint *y = b;
  const int c = strcmp(x->path, y->path);
  if (c != 0)
    return c;
  return x->line - y->line;
}

static void sortBreakpoints(void) {
  qsort(s_breakpoints, (size_t)s_count, sizeof(Breakpoint), compareBreakpoints);
}

static void freeBreakpoint(Breakpoint *bp) {
  gfree(bp->path);
  gfree(bp->condition);
  gfree(bp->hit_condition);
  gfree(bp->log_message);
}

static void removeAt(const int i) {
  freeBreakpoint(&s_breakpoints[i]);
  memmove(&s_breakpoints[i], &s_breakpoints[i + 1],
          sizeof(Breakpoint) * (size_t)(s_count - i - 1));
  s_count--;
}

static Breakpoint *add(const char *path, const int line) {
  Breakpoint *grown;
  Breakpoint *bp;
  if (s_count == s_cap) {
    grown = grealloc(s_breakpoints, sizeof(Breakpoint) * (size_t)(s_cap ? s_cap * 2 : 16));
    if (!grown)
      return NULL;
    s_breakpoints = grown;
    s_cap = s_cap ? s_cap * 2 : 16;
  }
  bp = &s_breakpoints[s_count];
  memset(bp, 0, sizeof(*bp));
  bp->path = utilStrdup(path);
  if (!bp->path)
    return NULL;
  bp->line = line;
  s_count++;
  sortBreakpoints();
  return breakpointAt(path, line);
}

Breakpoint *breakpointAt(const char *path, const int line) {
  int i;
  if (!path)
    return NULL;
  for (i = 0; i < s_count; i++)
    if (s_breakpoints[i].line == line && os_path_equal(s_breakpoints[i].path, path))
      return &s_breakpoints[i];
  return NULL;
}

int breakpointToggle(const char *path, const int line) {
  Breakpoint *bp;
  if (!path || line < 0)
    return -1;
  bp = breakpointAt(path, line);
  if (bp) {
    removeAt((int)(bp - s_breakpoints));
    notifyChanged(path);
    return 0;
  }
  if (!add(path, line))
    return -1;
  notifyChanged(path);
  return 1;
}

// Sets one of the breakpoint's texts (an offset into Breakpoint), adding
// the breakpoint if the line has none.
static int setText(const char *path, const int line, const size_t field, const char *text) {
  Breakpoint *bp;
  char **slot;
  if (!path || line < 0)
    return -1;
  bp = breakpointAt(path, line);
  if (!bp)
    bp = add(path, line);
  if (!bp)
    return -1;
  slot = (char **)((char *)bp + field);
  gfree(*slot);
  *slot = (text && *text) ? utilStrdup(text) : NULL;
  notifyChanged(path);
  return 0;
}

int breakpointSetCondition(const char *path, const int line, const char *condition) {
  return setText(path, line, offsetof(Breakpoint, condition), condition);
}

int breakpointSetHitCondition(const char *path, const int line, const char *hit_condition) {
  return setText(path, line, offsetof(Breakpoint, hit_condition), hit_condition);
}

int breakpointSetLogMessage(const char *path, const int line, const char *log_message) {
  return setText(path, line, offsetof(Breakpoint, log_message), log_message);
}

Breakpoint *breakpointsAll(int *count) {
  *count = s_count;
  return s_breakpoints;
}

int breakpointsInFile(const char *path) {
  int i;
  int n = 0;
  if (!path)
    return 0;
  for (i = 0; i < s_count; i++)
    if (os_path_equal(s_breakpoints[i].path, path))
      n++;
  return n;
}

int breakpointsClear(void) {
  Breakpoint *old = s_breakpoints;
  const int n = s_count;
  int i;
  s_breakpoints = NULL;
  s_count = 0;
  s_cap = 0;
  // Once the list is empty, so a running session clears them on its side.
  for (i = 0; i < n; i++)
    if (i == 0 || strcmp(old[i].path, old[i - 1].path) != 0)
      notifyChanged(old[i].path);
  for (i = 0; i < n; i++)
    freeBreakpoint(&old[i]);
  gfree(old);
  return n;
}

void breakpointsFree(void) {
  int i;
  gfree(s_store);
  s_store = NULL;
  for (i = 0; i < s_count; i++)
    freeBreakpoint(&s_breakpoints[i]);
  gfree(s_breakpoints);
  s_breakpoints = NULL;
  s_count = 0;
  s_cap = 0;
}

char *breakpointsBufferPath(const struct buffer_t *buf) {
  if (!buf || buf->type != FILE_BUFF || !buf->fullname)
    return NULL;
  return (char *)utf8ToString(buf->fullname);
}

void breakpointsLinesInserted(const struct buffer_t *buf, const int at, const int count) {
  char *path;
  int i;
  int moved = 0;
  if (s_count == 0 || count <= 0)
    return; // the usual case, and every row of a file being loaded
  path = breakpointsBufferPath(buf);
  if (!path)
    return;
  for (i = 0; i < s_count; i++) {
    if (s_breakpoints[i].line >= at && os_path_equal(s_breakpoints[i].path, path)) {
      s_breakpoints[i].line += count;
      moved = 1;
    }
  }
  if (moved)
    notifyChanged(path);
  gfree(path);
}

void breakpointsLineMoved(const struct buffer_t *buf, const int from, const int to) {
  char *path;
  Breakpoint *bp;
  if (s_count == 0 || from == to)
    return;
  path = breakpointsBufferPath(buf);
  if (!path)
    return;
  bp = breakpointAt(path, from);
  if (bp && !breakpointAt(path, to)) {
    bp->line = to;
    sortBreakpoints();
    notifyChanged(path);
  }
  gfree(path);
}

void breakpointsLinesRemoved(const struct buffer_t *buf, const int at, const int count) {
  char *path;
  int i;
  int moved = 0;
  int target;
  if (s_count == 0 || count <= 0)
    return;
  path = breakpointsBufferPath(buf);
  if (!path)
    return;
  target = at > 0 ? at - 1 : 0;
  for (i = 0; i < s_count; i++) {
    if (s_breakpoints[i].line < at || !os_path_equal(s_breakpoints[i].path, path))
      continue;
    s_breakpoints[i].line = s_breakpoints[i].line < at + count
                                ? target
                                : s_breakpoints[i].line - count;
    moved = 1;
  }
  if (moved) {
    // Two may now share a line (both rows joined into one): keep the first.
    sortBreakpoints();
    for (i = 1; i < s_count;) {
      if (s_breakpoints[i].line == s_breakpoints[i - 1].line &&
          os_path_equal(s_breakpoints[i].path, s_breakpoints[i - 1].path))
        removeAt(i);
      else
        i++;
    }
    notifyChanged(path);
  }
  gfree(path);
}

// ─── Kept across runs ────────────────────────────────────────────────────────
// One line per breakpoint:
//   "<line>\t<path>\t<condition>\t<hit condition>\t<log message>"
// the line 1-based as a person reads it, the texts possibly empty (and
// the last two missing in files written before they existed). Paths can't
// hold a tab or a newline here; the texts' tabs and newlines become spaces.

static void writeText(FILE *f, const char *text) {
  const char *c;
  fputc('\t', f);
  for (c = text; c && *c; c++)
    fputc((*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c, f);
}

// The next tab-separated field of *rest (NULL when there's none), cut off.
static char *nextField(char **rest) {
  char *field = *rest;
  char *tab;
  if (!field)
    return NULL;
  tab = strchr(field, '\t');
  if (tab)
    *tab++ = '\0';
  *rest = tab;
  return field;
}

int breakpointsSave(const char *file) {
  char tmp[OS_PATH_MAX];
  char err[256];
  FILE *f;
  int i;
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", file) >= (int)sizeof(tmp))
    return -1;
  f = fopen(tmp, "w");
  if (!f)
    return -1;
  for (i = 0; i < s_count; i++) {
    fprintf(f, "%d\t%s", s_breakpoints[i].line + 1, s_breakpoints[i].path);
    writeText(f, s_breakpoints[i].condition);
    writeText(f, s_breakpoints[i].hit_condition);
    writeText(f, s_breakpoints[i].log_message);
    fputc('\n', f);
  }
  if (os_file_flush(f) != 0) {
    fclose(f);
    remove(tmp);
    return -1;
  }
  fclose(f);
  if (os_replace_file(tmp, file, err, sizeof(err)) != 0) {
    remove(tmp);
    return -1;
  }
  return 0;
}

int breakpointsLoad(const char *file) {
  char line[OS_PATH_MAX + 1024];
  OsFileInfo info;
  FILE *f;
  char *path;
  char *condition;
  char *hit_condition;
  char *log_message;
  char *rest;
  char *end;
  size_t len;
  long n;
  int added = 0;
  f = fopen(file, "r");
  if (!f)
    return 0;
  s_loading = 1;
  while (fgets(line, sizeof(line), f)) {
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    n = strtol(line, &end, 10);
    if (end == line || *end != '\t' || n < 1)
      continue;
    rest = end + 1;
    path = nextField(&rest);
    condition = nextField(&rest);
    hit_condition = nextField(&rest);
    log_message = nextField(&rest);
    // A file that's gone (deleted, renamed) takes its breakpoints with it.
    if (!*path || breakpointAt(path, (int)n - 1) || os_file_info(path, &info) != 0)
      continue;
    if (breakpointSetCondition(path, (int)n - 1, condition) == 0)
      added++;
    breakpointSetHitCondition(path, (int)n - 1, hit_condition);
    breakpointSetLogMessage(path, (int)n - 1, log_message);
  }
  s_loading = 0;
  fclose(f);
  return added;
}

void breakpointsSetStore(const char *file) {
  gfree(s_store);
  s_store = NULL;
  if (!file)
    return;
  breakpointsLoad(file);
  s_store = utilStrdup(file);
}
