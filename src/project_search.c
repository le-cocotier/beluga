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
#include "../include/galloc.h"

#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/input.h"
#include "../include/split_screen.h"
#include "../include/subprocess.h"
#include "../include/utf8.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/**
 * @brief Is `root` inside a git work tree?
 * @details Decides between `git grep` (tracked files only, .gitignore
 * honored, binaries skipped) and plain `grep -r`. Cheap enough to ask git
 * directly rather than looking for a .git directory ourselves, which would
 * get worktrees and submodules wrong.
 */
static int rootIsGitRepo(const char *root) {
  char qroot[PATH_MAX + 4];
  char cmd[PATH_MAX + 96];
  char *out;
  int yes;
  shellQuote(root, qroot, sizeof(qroot));

  snprintf(cmd, sizeof(cmd),
           "git -C %s rev-parse --is-inside-work-tree 2>/dev/null", qroot);

  out = runCommandCapture(cmd, 1000, 64);
  if (!out)
    return 0;
  yes = (strncmp(out, "true", 4) == 0);
  gfree(out);
  return yes;
}

/**
 * @brief Runs the search and returns grep's raw "path:line:text" output.
 * @details -F searches for the pattern literally: someone typing "a[0]"
 * into a search prompt means those four characters, not a regex character
 * class. -I skips binary files in both tools.
 */
static char *runSearch(const char *pattern, const char *root) {
  char qpat[SEARCH_MAX_PATTERN * 4 + 8];
  char qroot[PATH_MAX + 4];
  char cmd[PATH_MAX * 2 + SEARCH_MAX_PATTERN * 4 + 256];
  shellQuote(pattern, qpat, sizeof(qpat));
  shellQuote(root, qroot, sizeof(qroot));

  if (rootIsGitRepo(root)) {
    snprintf(cmd, sizeof(cmd),
             "git -C %s grep -I -n --no-color -F -e %s 2>/dev/null", qroot,
             qpat);
  } else {
    snprintf(cmd, sizeof(cmd),
             "grep -rnI --exclude-dir=.git --exclude-dir=build "
             "--exclude-dir=node_modules -F -e %s %s 2>/dev/null",
             qpat, qroot);
  }

  return runCommandCapture(cmd, SEARCH_TIMEOUT_MS, SEARCH_MAX_OUTPUT);
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
  for (line = strtok_r(output, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    if (s_hit_count >= SEARCH_MAX_HITS) {
      s_truncated = 1;
      break;
    }

    path = NULL; text = NULL;
    line_no = 0;
    if (!parseGrepLine(line, &path, &line_no, &text))
      continue;

    // git grep reports paths relative to the repo root, plain grep
    // relative to (or under) whatever root it was handed. Store both
    // forms: absolute is what opens the file, relative is what reads
    // well in a narrow pane.
    if (path[0] == '/')
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
    hit->abs_path = strdup(abs);
    hit->rel_path = strdup(rel);
    hit->line = line_no;
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
 * @brief Fills the results buffer, grouping consecutive hits under the
 * file they came from.
 * @details Grep repeats the path on every line; a file heading plus bare
 * line numbers says the same thing in far less width, which matters as
 * soon as the results share the screen with the file you're jumping into.
 */
static void renderResults(Buffer *buf, const char *pattern) {
  char header[SEARCH_MAX_PATTERN + 128];
  int files;
  int i;
  char row[SEARCH_MAX_TEXT + 32];
  int r;
  clearBufferRows(buf);
  gfree(s_row_hit);
  s_row_hit = NULL;
  s_row_count = 0;

  files = 0;
  for (i = 0; i < s_hit_count; i++)
    if (i == 0 || strcmp(s_hits[i].rel_path, s_hits[i - 1].rel_path) != 0)
      files++;

  snprintf(header, sizeof(header), "%d match%s for \"%s\" in %d file%s%s",
           s_hit_count, s_hit_count == 1 ? "" : "es", pattern, files,
           files == 1 ? "" : "s", s_truncated ? " (truncated)" : "");
  addRow(buf, header, -1);
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
  size_t rlen;
  char *output;
  Buffer *buf;
  EditorPane *active;
  struct prefix_t prefix;
  if (!pattern || !*pattern)
    return -1;

  if (root && *root)
    snprintf(resolved, sizeof(resolved), "%s", root);
  else if (!getcwd(resolved, sizeof(resolved)))
    return -1;

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
  renderResults(buf, pattern);

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
  target->x = 0;

  // Leave the scroll offset to editorScroll() on the next redraw, same
  // as editor-goto-line — it's the one place that knows the pane's
  // height and gutter width.
  E.prefix_state = 0;
  editorSetStatusMessage("%s:%d", hit->rel_path, hit->line);
}
