/**
 * @file project_search.c
 * @brief Project-wide search: grep the tree, list the hits in a buffer,
 * jump from a hit to the file and line it came from.
 * @details The search itself shells out rather than walking the tree in C.
 * `git grep` already knows which files are worth searching (tracked ones,
 * so .gitignore is honored for free) and skips binaries; plain `grep -r`
 * covers the non-repo case. Reimplementing either — directory recursion,
 * binary sniffing, ignore rules — would be a lot of code to arrive back at
 * what's already installed on both platforms the editor targets.
 *
 * Results live in an ordinary read-only scratch buffer, so scrolling,
 * splitting and buffer switching all work on it without special cases.
 * The one piece of special behavior is the buffer's focus_prefix: landing
 * in this buffer activates the "search" prefix, which is where ENTER is
 * bound to "jump to the result under the cursor".
 */

#include "../include/project_search.h"
#include "../include/define.h"
#include "../include/galloc.h"

#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/input.h"
#include "../include/project_files.h"
#include "../include/split_screen.h"
#include "../include/subprocess.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

#define SEARCH_BUFFER_NAME "*search*"
#define SEARCH_PREFIX_NAME "search"

// A search that matches half the tree is a mistake, not a query — these
// caps keep one from turning into a multi-second freeze or a buffer with
// more rows than the file it came from.
#define SEARCH_MAX_HITS 2000
#define SEARCH_MAX_OUTPUT (4 * 1024 * 1024)
#define SEARCH_TIMEOUT_MS 5000
#define SEARCH_MAX_TEXT 300
#define SEARCH_MAX_PATTERN 256

typedef struct {
  char *abs_path; // absolute, for opening the file
  char *rel_path; // relative to the search root, for display
  int line;       // 1-based, as grep reports it
  int col;        // 0-based character to land on (0 for grep hits)
  char *text;     // the matching line, tabs expanded and trimmed
} SearchHit;

static SearchHit *s_hits = NULL;
static int s_hit_count = 0;

// results-buffer row -> index into s_hits, or -1 for a row that isn't a
// result (the header, a blank spacer, a file-group heading). Built while
// rendering rather than derived from a fixed header height, so the layout
// can change without the jump target quietly drifting with it.
static int *s_row_hit = NULL;
static int s_row_count = 0;

static int s_results_buffer_id = -1;
static char s_root[PATH_MAX] = "";
static int s_truncated = 0;
// The results buffer's first line. Grep results describe their own query;
// other lists (LSP references, diagnostics) bring theirs.
static char s_title[SEARCH_MAX_PATTERN + 128] = "";

// ─── Result storage ─────────────────────────────────────────────────────────

static void clearHits(void) {
  int i;
  for (i = 0; i < s_hit_count; i++) {
    gfree(s_hits[i].abs_path);
    gfree(s_hits[i].rel_path);
    gfree(s_hits[i].text);
  }
  gfree(s_hits);
  s_hits = NULL;
  s_hit_count = 0;

  gfree(s_row_hit);
  s_row_hit = NULL;
  s_row_count = 0;
}

void projectSearchFree(void) { clearHits(); }

// ─── Building the command ───────────────────────────────────────────────────

// ─── Searching without git ──────────────────────────────────────────────────

typedef struct {
  const char *pattern;
  size_t pattern_len;
  long long deadline;
  char *out; // "path:line:text\n"..., galloc'd
  size_t len;
  size_t cap;
  int stop; // cap or deadline reached
} PlainSearch;

static void plainSearchAppend(PlainSearch *ps, const char *rel,
                              const int line_no, const char *text,
                              const size_t text_len) {
  char prefix[OS_PATH_MAX + 32];
  const int prefix_len =
      snprintf(prefix, sizeof(prefix), "%s:%d:", rel, line_no);
  const size_t need = (size_t)prefix_len + text_len + 2;
  char *grown;
  size_t cap;

  if (ps->len + need > SEARCH_MAX_OUTPUT) {
    ps->stop = 1;
    return;
  }
  if (ps->len + need > ps->cap) {
    cap = ps->cap ? ps->cap : 4096;
    while (ps->len + need > cap)
      cap *= 2;
    grown = grealloc(ps->out, cap);
    if (!grown) {
      ps->stop = 1;
      return;
    }
    ps->out = grown;
    ps->cap = cap;
  }
  memcpy(ps->out + ps->len, prefix, (size_t)prefix_len);
  ps->len += (size_t)prefix_len;
  memcpy(ps->out + ps->len, text, text_len);
  ps->len += text_len;
  ps->out[ps->len++] = '\n';
  ps->out[ps->len] = '\0';
}

/**
 * @brief Searches one file: every line containing the pattern, literally.
 * Files with a NUL byte in their first 8 KiB are binary and skipped, as
 * grep -I and git grep -I do.
 */
static int plainSearchFile(const char *rel, const char *full, void *ctx) {
  PlainSearch *ps = ctx;
  FILE *f;
  char *data;
  long size;
  size_t got;
  const char *line;
  const char *end;
  const char *stop;
  size_t line_len;
  int line_no = 0;

  if (os_monotonic_ms() > ps->deadline) {
    ps->stop = 1;
    return 1;
  }
  f = fopen(full, "rb");
  if (!f)
    return 0;
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
      size > 64L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }
  data = galloc((size_t)size);
  got = data ? fread(data, 1, (size_t)size, f) : 0;
  fclose(f);
  if (!data || memchr(data, '\0', got < 8192 ? got : 8192)) {
    free(data);
    return 0;
  }

  stop = data + got;
  for (line = data; line < stop && !ps->stop; line = end + 1) {
    end = memchr(line, '\n', (size_t)(stop - line));
    if (!end)
      end = stop;
    line_no++;
    line_len = (size_t)(end - line);
    if (line_len > 0 && line[line_len - 1] == '\r')
      line_len--;
    if (utilMemmem(line, line_len, ps->pattern, ps->pattern_len))
      plainSearchAppend(ps, rel, line_no, line, line_len);
  }
  free(data);
  return ps->stop;
}

/**
 * @brief Runs the search and returns raw "path:line:text" output, paths
 * relative to `root`.
 * @details The pattern is literal: someone typing "a[0]" into a search
 * prompt means those four characters, not a regex character class. In a
 * git repository, git grep: tracked files only, .gitignore honored,
 * binaries skipped (-I). Elsewhere, the same search done here, over every
 * file the project walker visits.
 */
static char *runSearch(const char *pattern, const char *root) {
  char *argv[] = {"git",        "-C", (char *)root, "grep",          "-I", "-n",
                  "--no-color", "-F", "-e",         (char *)pattern, NULL};
  PlainSearch ps;

  if (projectIsGitRepo(root))
    return runArgvCaptureQuiet(argv, SEARCH_TIMEOUT_MS, SEARCH_MAX_OUTPUT,
                               NULL);

  memset(&ps, 0, sizeof(ps));
  ps.pattern = pattern;
  ps.pattern_len = strlen(pattern);
  ps.deadline = os_monotonic_ms() + SEARCH_TIMEOUT_MS;
  if (ps.pattern_len == 0)
    return NULL;
  projectWalkFiles(root, plainSearchFile, &ps);
  return ps.out;
}

// ─── Parsing ────────────────────────────────────────────────────────────────

/**
 * @brief Splits one grep output line into path, line number and text.
 * @details Scans for the first ":<digits>:" rather than just the first two
 * colons, so a path that itself contains a colon doesn't shift everything
 * one field to the left.
 * @return 1 on success (with *out_* filled, pointing into `line`), 0 if
 * the line isn't in grep's format at all.
 */
static int parseGrepLine(char *line, char **out_path, int *out_line,
                         char **out_text) {
  char *colon;
  char *p;
  int number;
  for (colon = strchr(line, ':'); colon; colon = strchr(colon + 1, ':')) {
    p = colon + 1;
    if (!isdigit((unsigned char)*p))
      continue;

    number = 0;
    while (isdigit((unsigned char)*p)) {
      number = number * 10 + (*p - '0');
      p++;
    }
    if (*p != ':')
      continue; // digits, but not a "path:line:" field

    *colon = '\0';
    *out_path = line;
    *out_line = number;
    *out_text = p + 1;
    return 1;
  }
  return 0;
}

/**
 * @brief Copies `src` into a fresh string with tabs expanded to a single
 * space, leading whitespace dropped and the whole thing capped at
 * SEARCH_MAX_TEXT characters.
 * @details A matching line is shown for recognition, not for editing —
 * its own indentation would just push the interesting part off the right
 * edge, and a minified or generated file could otherwise contribute a
 * single row thousands of columns wide.
 */
static char *cleanText(const char *src) {
  char *out;
  size_t n;
  unsigned char c;
  while (*src == ' ' || *src == '\t')
    src++;

  out = galloc(SEARCH_MAX_TEXT + 1);
  if (!out)
    return NULL;

  n = 0;
  for (; *src && n < SEARCH_MAX_TEXT; src++) {
    c = (unsigned char)*src;
    if (c == '\t')
      out[n++] = ' ';
    else if (c < 0x20)
      ; // stray control bytes would corrupt the row — skip it
    else
      out[n++] = (char)c;
  }
  out[n] = '\0';
  return out;
}

static int parseResults(char *output, const char *root) {
  const size_t root_len = strlen(root);

  char *saveptr = NULL;
  char *line;
  char *path;
  char *text;
  int line_no;
  char abs[PATH_MAX];
  const char *rel;
  SearchHit *grown;
  SearchHit *hit;
  for (line = utilStrtok(output, "\n", &saveptr); line;
       line = utilStrtok(NULL, "\n", &saveptr)) {
    if (s_hit_count >= SEARCH_MAX_HITS) {
      s_truncated = 1;
      break;
    }

    path = NULL;
    text = NULL;
    line_no = 0;
    if (!parseGrepLine(line, &path, &line_no, &text))
      continue;

    // git grep reports paths relative to the repo root, plain grep
    // relative to (or under) whatever root it was handed. Store both
    // forms: absolute is what opens the file, relative is what reads
    // well in a narrow pane.
    if (os_path_is_absolute(path))
      snprintf(abs, sizeof(abs), "%s", path);
    else
      snprintf(abs, sizeof(abs), "%s/%s", root, path);

    rel = abs;
    if (strncmp(abs, root, root_len) == 0 && abs[root_len] == '/')
      rel = abs + root_len + 1;

    grown = grealloc(s_hits, (size_t)(s_hit_count + 1) * sizeof(SearchHit));
    if (!grown)
      break;
    s_hits = grown;

    hit = &s_hits[s_hit_count];
    hit->abs_path = utilStrdup(abs);
    hit->rel_path = utilStrdup(rel);
    hit->line = line_no;
    hit->col = 0;
    hit->text = cleanText(text);

    if (!hit->abs_path || !hit->rel_path || !hit->text) {
      gfree(hit->abs_path);
      gfree(hit->rel_path);
      gfree(hit->text);
      break;
    }
    s_hit_count++;
  }

  return s_hit_count;
}

// ─── Rendering ──────────────────────────────────────────────────────────────

/**
 * @brief Appends one line to the results buffer, recording which hit (if
 * any) that row jumps to.
 */
static void addRow(Buffer *buf, const char *text, const int hit_index) {
  int *grown = grealloc(s_row_hit, (size_t)(s_row_count + 1) * sizeof(int));
  Utf8Char *u;
  if (!grown)
    return;
  s_row_hit = grown;
  s_row_hit[s_row_count++] = hit_index;

  u = stringToUtf8((unsigned char *)text);
  if (!u)
    return;
  bufferInsertRow(buf, buf->numrows, u, utf8StringLen(u) - 1);
  gfree(u);
}

static void clearBufferRows(Buffer *buf) {
  int i;
  for (i = 0; i < buf->numrows; i++) {
    gfree(buf->row[i].characters);
    gfree(buf->row[i].attrs);
  }
  gfree(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
  buf->x = 0;
  buf->y = 0;
}

/**
 * @brief Number of distinct files among the hits (consecutive grouping, as
 * displayed).
 */
static int countFiles(void) {
  int files = 0;
  int i;
  for (i = 0; i < s_hit_count; i++)
    if (i == 0 || strcmp(s_hits[i].rel_path, s_hits[i - 1].rel_path) != 0)
      files++;
  return files;
}

/**
 * @brief Fills the results buffer, grouping consecutive hits under the
 * file they came from.
 * @details Grep repeats the path on every line; a file heading plus bare
 * line numbers says the same thing in far less width, which matters as
 * soon as the results share the screen with the file you're jumping into.
 */
static void renderResults(Buffer *buf) {
  int i;
  char row[SEARCH_MAX_TEXT + 32];
  int r;
  clearBufferRows(buf);
  gfree(s_row_hit);
  s_row_hit = NULL;
  s_row_count = 0;

  addRow(buf, s_title, -1);
  addRow(buf, "", -1);

  for (i = 0; i < s_hit_count; i++) {
    if (i == 0 || strcmp(s_hits[i].rel_path, s_hits[i - 1].rel_path) != 0) {
      if (i > 0)
        addRow(buf, "", -1);
      addRow(buf, s_hits[i].rel_path, -1);
    }

    snprintf(row, sizeof(row), "  %5d  %s", s_hits[i].line, s_hits[i].text);
    addRow(buf, row, i);
  }

  buf->dirty = 0;

  // Land on the first actual result rather than the header, so ENTER
  // does something useful the moment the buffer appears.
  for (r = 0; r < s_row_count; r++) {
    if (s_row_hit[r] >= 0) {
      buf->y = r;
      break;
    }
  }
}

// ─── Public entry points ────────────────────────────────────────────────────

static Buffer *openResultsBuffer(void) {
  Buffer *buf = bufferFindById(s_results_buffer_id);
  Utf8Char *name;
  if (!buf || buf->type != EXPLORER_BUFF) {
    name = stringToUtf8((unsigned char *)SEARCH_BUFFER_NAME);
    if (!name)
      return NULL;
    s_results_buffer_id = bufferCreate(name, READ_ONLY, EXPLORER_BUFF);
    gfree(name);
    buf = bufferFindById(s_results_buffer_id);
    if (!buf)
      return NULL;
    snprintf(buf->focus_prefix, sizeof(buf->focus_prefix), "%s",
             SEARCH_PREFIX_NAME);
  }
  return buf;
}

int projectSearchRun(const char *pattern, const char *root) {
  char resolved[PATH_MAX];
  char *cwd;
  size_t rlen;
  char *output;
  Buffer *buf;
  EditorPane *active;
  struct prefix_t prefix;
  if (!pattern || !*pattern)
    return -1;

  if (root && *root)
    snprintf(resolved, sizeof(resolved), "%s", root);
  else {
    cwd = os_getcwd();
    if (!cwd)
      return -1;
    snprintf(resolved, sizeof(resolved), "%s", cwd);
    free(cwd);
  }

  // Strip a trailing slash so the "is this path under the root" prefix
  // test below (and the rel_path it produces) doesn't double up on it.
  rlen = strlen(resolved);
  while (rlen > 1 && resolved[rlen - 1] == '/')
    resolved[--rlen] = '\0';
  snprintf(s_root, sizeof(s_root), "%s", resolved);

  clearHits();
  s_truncated = 0;

  output = runSearch(pattern, resolved);
  if (!output) {
    editorSetStatusMessage("No matches for \"%s\"", pattern);
    return 0;
  }

  parseResults(output, resolved);
  gfree(output);

  if (s_hit_count == 0) {
    editorSetStatusMessage("No matches for \"%s\"", pattern);
    return 0;
  }

  buf = openResultsBuffer();
  if (!buf)
    return -1;
  snprintf(s_title, sizeof(s_title), "%d match%s for \"%s\" in %d file%s%s",
           s_hit_count, s_hit_count == 1 ? "" : "es", pattern, countFiles(),
           countFiles() == 1 ? "" : "s", s_truncated ? " (truncated)" : "");
  renderResults(buf);

  active = splitScreenGetActivePane();
  active->buffer_id = s_results_buffer_id;
  active->cursor_y = buf->y;
  active->cursor_x = 0;
  active->y_offset = 0;
  active->x_offset = 0;

  prefix = find_prefix(SEARCH_PREFIX_NAME);
  if (prefix.prefix_id != -1)
    E.prefix_state = prefix.prefix_id;

  editorSetStatusMessage("%d match%s for \"%s\"%s — ENTER to jump", s_hit_count,
                         s_hit_count == 1 ? "" : "es", pattern,
                         s_truncated ? " (truncated)" : "");
  return s_hit_count;
}

void projectSearchPrompt(void) {
  Utf8Char *input = editorPrompt("Search project: %u (ESC to cancel)", NULL, 0);
  char *pattern;
  if (!input)
    return;

  pattern = (char *)utf8ToString(input);
  gfree(input);
  if (!pattern)
    return;

  if (*pattern)
    projectSearchRun(pattern, NULL);
  gfree(pattern);
}

/**
 * @brief Finds an already-open buffer for `abs_path`, so repeatedly
 * jumping into the same file doesn't stack up a new buffer every time.
 * @details Compares full paths: bufferFindByFilename() only knows
 * basenames, which would happily hand back the wrong init.lisp.
 */
static int findOpenBufferByPath(const char *abs_path) {
  int first;
  Buffer *b;
  char *full;
  int same;
  if (E.buffers == NULL)
    return -1;

  first = E.buffers->buffer_id;
  b = E.buffers;
  do {
    if (b->type == FILE_BUFF && b->fullname) {
      full = (char *)utf8ToString(b->fullname);
      if (full) {
        same = (strcmp(full, abs_path) == 0);
        gfree(full);
        if (same)
          return b->buffer_id;
      }
    }
    b = b->next;
  } while (b->buffer_id != first);

  return -1;
}

void projectSearchGotoCursor(void) {
  EditorPane *active = splitScreenGetActivePane();
  Buffer *results = bufferFindById(active->buffer_id);
  int row;
  int index;
  int r;
  SearchHit *hit;
  int existing;
  Utf8Char *upath;
  int id;
  Buffer *target;
  int line;
  if (!results || results->buffer_id != s_results_buffer_id) {
    editorSetStatusMessage("Not in the search results buffer");
    return;
  }

  row = results->y;
  if (row < 0 || row >= s_row_count || s_row_hit == NULL)
    return;

  // The cursor can legitimately be sitting on a row that isn't itself a
  // result — the header, a blank spacer, a file heading. Rather than do
  // nothing (which just reads as ENTER being broken), take the nearest
  // result: forward first, so ENTER on a file heading opens that file's
  // first hit, then backward for the trailing rows below the last one.
  index = s_row_hit[row];
  if (index < 0) {
    for (r = row + 1; r < s_row_count && index < 0; r++)
      index = s_row_hit[r];
    for (r = row - 1; r >= 0 && index < 0; r--)
      index = s_row_hit[r];
  }
  if (index < 0 || index >= s_hit_count)
    return; // no results at all

  hit = &s_hits[index];

  // With the screen already split, open into the other pane so the
  // result list stays on screen — the same "list on one side, file on
  // the other" arrangement the explorer sidebar uses. Unsplit, there's
  // nowhere else to put it, so it replaces the results in place (they're
  // still one buffer switch away).
  //
  // splitScreenFocusOtherRealPane() rather than a plain pane-rotate: with
  // the explorer sidebar also open (results + sidebar + this pane, or
  // just results + sidebar), a blind "next pane" could land on the
  // sidebar itself and silently overwrite the tree with the opened file.
  if (splitScreenFocusOtherRealPane())
    active = splitScreenGetActivePane();

  existing = findOpenBufferByPath(hit->abs_path);
  if (existing != -1) {
    active->buffer_id = existing;
  } else {
    upath = stringToUtf8((unsigned char *)hit->abs_path);
    if (!upath)
      return;
    id = bufferCreate(upath, READ_AND_WRITE, FILE_BUFF);
    gfree(upath);
    if (id == -1) {
      editorSetStatusMessage("Could not open %s", hit->rel_path);
      return;
    }
    active->buffer_id = id;
  }

  target = bufferFindById(active->buffer_id);
  if (!target)
    return;

  line = hit->line - 1; // grep counts from 1, buffers from 0
  if (line < 0)
    line = 0;
  if (line >= target->numrows)
    line = target->numrows > 0 ? target->numrows - 1 : 0;
  target->y = line;
  target->x = hit->col;
  if (target->numrows > 0 && target->x > target->row[line].size)
    target->x = target->row[line].size;

  // Leave the scroll offset to editorScroll() on the next redraw, same
  // as editor-goto-line — it's the one place that knows the pane's
  // height and gutter width.
  E.prefix_state = 0;
  editorSetStatusMessage("%s:%d", hit->rel_path, hit->line);
}

// ─── Other location lists ───────────────────────────────────────────────────

/**
 * @brief Shows `n` locations in the results buffer, under `title`, with the
 * same grouping by file and ENTER-to-jump as a project search.
 * @details Paths are shown relative to the working directory when they're
 * under it. Replaces whatever results were shown before.
 */
void projectSearchShowLocations(const char *title, const SearchLocation *locs,
                                const int n) {
  char *cwd;
  size_t cwd_len;
  const char *rel;
  SearchHit *grown;
  SearchHit *hit;
  Buffer *buf;
  EditorPane *active;
  struct prefix_t prefix;
  int i;

  clearHits();
  s_truncated = 0;
  cwd = os_getcwd();
  cwd_len = cwd ? strlen(cwd) : 0;
  snprintf(s_root, sizeof(s_root), "%s", cwd ? cwd : "");

  for (i = 0; i < n && i < SEARCH_MAX_HITS; i++) {
    grown = grealloc(s_hits, (size_t)(s_hit_count + 1) * sizeof(SearchHit));
    if (!grown)
      break;
    s_hits = grown;
    rel = locs[i].abs_path;
    if (cwd && strncmp(rel, cwd, cwd_len) == 0 && rel[cwd_len] == '/')
      rel += cwd_len + 1;
    hit = &s_hits[s_hit_count];
    hit->abs_path = utilStrdup(locs[i].abs_path);
    hit->rel_path = utilStrdup(rel);
    hit->line = locs[i].line;
    hit->col = locs[i].col;
    hit->text = cleanText(locs[i].text ? locs[i].text : "");
    if (!hit->abs_path || !hit->rel_path || !hit->text) {
      gfree(hit->abs_path);
      gfree(hit->rel_path);
      gfree(hit->text);
      break;
    }
    s_hit_count++;
  }
  s_truncated = n > s_hit_count;
  free(cwd);

  snprintf(s_title, sizeof(s_title), "%s%s", title,
           s_truncated ? " (truncated)" : "");
  buf = openResultsBuffer();
  if (!buf)
    return;
  renderResults(buf);

  active = splitScreenGetActivePane();
  active->buffer_id = s_results_buffer_id;
  active->cursor_y = buf->y;
  active->cursor_x = 0;
  active->y_offset = 0;
  active->x_offset = 0;
  prefix = find_prefix(SEARCH_PREFIX_NAME);
  E.prefix_state = prefix.prefix_id;
}

// ─── For project-wide replace ───────────────────────────────────────────────

int projectSearchFiles(const char *pattern, const char *root,
                       char ***out_files) {
  char resolved[PATH_MAX];
  char abs[PATH_MAX];
  char *cwd;
  char *output;
  char *saveptr = NULL;
  char *line;
  char *path;
  char *text;
  int line_no;
  char **files = NULL;
  char **grown;
  int n = 0;
  int i;
  int dup;
  size_t rlen;

  *out_files = NULL;
  if (!pattern || !*pattern)
    return -1;
  if (root && *root)
    snprintf(resolved, sizeof(resolved), "%s", root);
  else {
    cwd = os_getcwd();
    if (!cwd)
      return -1;
    snprintf(resolved, sizeof(resolved), "%s", cwd);
    free(cwd);
  }
  rlen = strlen(resolved);
  while (rlen > 1 && resolved[rlen - 1] == '/')
    resolved[--rlen] = '\0';

  // Same search as project-search (and the same caps), but the hits stay
  // out of s_hits: the *search* buffer keeps pointing at its own results.
  output = runSearch(pattern, resolved);
  if (!output)
    return 0;

  for (line = utilStrtok(output, "\n", &saveptr); line;
       line = utilStrtok(NULL, "\n", &saveptr)) {
    if (!parseGrepLine(line, &path, &line_no, &text))
      continue;
    if (os_path_is_absolute(path))
      snprintf(abs, sizeof(abs), "%s", path);
    else
      snprintf(abs, sizeof(abs), "%s/%s", resolved, path);

    // grep lists a file's hits together, but check them all anyway.
    dup = 0;
    for (i = n - 1; i >= 0 && !dup; i--)
      dup = strcmp(files[i], abs) == 0;
    if (dup)
      continue;

    grown = grealloc(files, (size_t)(n + 1) * sizeof(char *));
    if (!grown)
      break;
    files = grown;
    files[n] = utilStrdup(abs);
    if (!files[n])
      break;
    n++;
  }
  gfree(output);

  *out_files = files;
  return n;
}

int projectSearchOpenFile(const char *abs_path) {
  int existing = findOpenBufferByPath(abs_path);
  Utf8Char *upath;
  int id;
  if (existing != -1)
    return existing;
  upath = stringToUtf8((unsigned char *)abs_path);
  if (!upath)
    return -1;
  id = bufferCreate(upath, READ_AND_WRITE, FILE_BUFF);
  gfree(upath);
  return id;
}
