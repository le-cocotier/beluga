/**
 * @file file_finder.c
 * @brief (find-file) — a fuzzy project-file picker, modal popup style.
 * @details Snapshots every file in the project once when it opens (git
 * ls-files inside a repo, a pruned find(1) otherwise), then narrows the
 * list as you type and opens the highlighted file on ENTER. Kept separate
 * from list_popup.c on purpose: that one walks a Lisp list, which is fine
 * for the handful of entries an outline or an environment menu has, but a
 * repo can hand us thousands of paths and this one re-scores all of them
 * on every keystroke — a flat char* array and a plain C loop, no Lisp
 * allocation in the hot path.
 *
 * Visually it reuses popup_ui.c, so it matches the buffer switcher and
 * the completion popup; structurally the draw loop is the same
 * scroll-window logic list_popup.c uses.
 */

#include "../include/file_finder.h"
#include "../include/galloc.h"
#include "../include/buffer.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/popup_ui.h"
#include "../include/project.h"
#include "../include/split_screen.h"
#include "../include/project_files.h"
#include "../include/subprocess.h"
#include "../include/utils.h"
#include "../include/utf8.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

#define FF_LIST_CMD_TIMEOUT_MS 4000
#define FF_MAX_OUTPUT (8 * 1024 * 1024)
#define FF_MAX_FILES 50000
#define FF_VISIBLE_ROWS LIST_POPUP_MAX_H

// ─── Building the file list ─────────────────────────────────────────────────

typedef struct {
  char *text; // newline-separated, galloc'd
  size_t len;
  size_t cap;
  int full;   // FF_MAX_OUTPUT reached
} PathList;

static void pathListAdd(PathList *l, const char *path) {
  const size_t n = strlen(path);
  char *grown;
  size_t cap;
  if (l->full)
    return;
  if (l->len + n + 2 > FF_MAX_OUTPUT) {
    l->full = 1;
    return;
  }
  if (l->len + n + 2 > l->cap) {
    cap = l->cap ? l->cap : 4096;
    while (l->len + n + 2 > cap)
      cap *= 2;
    grown = grealloc(l->text, cap);
    if (!grown) {
      l->full = 1;
      return;
    }
    l->text = grown;
    l->cap = cap;
  }
  memcpy(l->text + l->len, path, n);
  l->len += n;
  l->text[l->len++] = '\n';
  l->text[l->len] = '\0';
}

static int comparePaths(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b); // LC_ALL=C order
}

/**
 * @brief Sorts the `len` bytes of `text`, entries separated by `sep`,
 * byte-wise and drops duplicates (LC_ALL=C sort -u). Returns a galloc'd,
 * newline-separated copy; consumes `text`.
 */
static char *sortUniqueLines(char *text, const size_t len, const char sep) {
  size_t count = 0;
  size_t cap = 1024;
  char **lines = galloc(cap * sizeof(char *));
  char **grown;
  char *p = text;
  char *const stop = text + len;
  char *end;
  PathList out = {NULL, 0, 0, 0};
  size_t i;

  if (!lines) {
    gfree(text);
    return NULL;
  }
  while (p < stop) {
    end = memchr(p, sep, (size_t)(stop - p));
    if (!end)
      end = stop; // text[len] is the NUL terminator
    *end = '\0';
    if (*p) {
      if (count == cap) {
        cap *= 2;
        grown = grealloc(lines, cap * sizeof(char *));
        if (!grown)
          break;
        lines = grown;
      }
      lines[count++] = p;
    }
    p = end + 1;
  }
  qsort(lines, count, sizeof(char *), comparePaths);
  for (i = 0; i < count; i++)
    if (i == 0 || strcmp(lines[i], lines[i - 1]) != 0)
      pathListAdd(&out, lines[i]);
  gfree(lines);
  gfree(text);
  return out.text;
}

static int addWalkedFile(const char *rel_path, const char *full_path, void *ctx) {
  PathList *list = ctx;
  (void)full_path;
  pathListAdd(list, rel_path);
  return list->full;
}

/**
 * @brief The project's files, newline-separated and relative to `root`,
 * sorted (galloc'd), or NULL.
 * @details In a git repository: tracked files plus untracked-but-not-
 * ignored ones (so a file you just created shows up), NUL-separated from
 * git so no file name needs quoting. Elsewhere: every file under the
 * root, the usual heavy directories skipped.
 */
static char *listProjectFiles(const char *root) {
  char *argv[] = {"git", "-C", (char *)root, "ls-files", "--cached", "--others",
                  "--exclude-standard", "-z", NULL};
  PathList list = {NULL, 0, 0, 0};
  size_t len = 0;
  char *out;

  if (projectIsGitRepo(root)) {
    out = runArgvCaptureBytes(argv, FF_LIST_CMD_TIMEOUT_MS, FF_MAX_OUTPUT, &len);
    return out ? sortUniqueLines(out, len, '\0') : NULL;
  }
  projectWalkFiles(root, addWalkedFile, &list);
  return list.text ? sortUniqueLines(list.text, list.len, '\n') : NULL;
}

/**
 * @brief Splits `output` (newline-separated) into E.file_finder.paths,
 * stripping a leading "./" from each (find prints it, git doesn't).
 * Consumes `output` by writing NULs into it; the copied-out paths are
 * freshly malloc'd.
 */
static void ownPaths(char *output) {
  FileFinder *ff = &E.file_finder;

  int cap = 256;
  char *saveptr;
  char *line;
  char **grown;
  ff->paths = galloc((size_t)cap * sizeof(char *));
  ff->path_count = 0;
  if (!ff->paths)
    return;

  saveptr = NULL;
  for (line = utilStrtok(output, "\n", &saveptr);
       line && ff->path_count < FF_MAX_FILES;
       line = utilStrtok(NULL, "\n", &saveptr)) {
    if (line[0] == '.' && line[1] == '/')
      line += 2;
    if (line[0] == '\0')
      continue;

    if (ff->path_count == cap) {
      cap *= 2;
      grown = grealloc(ff->paths, (size_t)cap * sizeof(char *));
      if (!grown)
        break;
      ff->paths = grown;
    }
    ff->paths[ff->path_count] = utilStrdup(line);
    if (!ff->paths[ff->path_count])
      break;
    ff->path_count++;
  }
}

// ─── Fuzzy matching ────────────────────────────────────────────────────────

/**
 * @brief Case-insensitive subsequence score of `query` within `path`.
 * @return -1 if `query` is not a subsequence of `path` at all, otherwise
 * a score where higher is a better match. An empty query scores 0 for
 * every path (so the list shows in its original order until you type).
 * @details Rewards matches that are consecutive and that land on a word
 * boundary (start of string, or just after '/', '_', '-', '.', ' '), so
 * "bufc" ranks "src/buffer.c" above "a/subfolder/cache".
 */
static int fuzzyScore(const char *query, const char *path) {
  int score;
  int run;
  const char *p;
  char prev;
  const char *q;
  int qc;
  if (!query[0])
    return 0;

  score = 0;
  run = 0; // current consecutive-match streak
  p = path;
  prev = '/'; // pretend a separator precedes the path

  for (q = query; *q; q++) {
    qc = tolower((unsigned char)*q);
    while (*p && tolower((unsigned char)*p) != qc) {
      prev = *p;
      p++;
      run = 0;
    }
    if (!*p)
      return -1; // ran out of path before matching this query char

    score += 1;
    if (run > 0)
      score += 8; // consecutive
    if (prev == '/')
      score += 12; // start of a path segment
    else if (prev == '_' || prev == '-' || prev == '.' || prev == ' ')
      score += 6; // word boundary

    run++;
    prev = *p;
    p++;
  }

  // A shorter tail after the last match means the query covered more of
  // the (usually more meaningful) basename.
  score -= (int)strlen(p) / 4;
  return score;
}

// Score per path, indexed like paths[]: set for the length of one
// rebuildMatches() call, for compareByScore(). Not kept between calls,
// since each daemon client has its own finder with its own path count.
static int *s_score_scratch = NULL;

static int compareByScore(const void *a, const void *b) {
  const int ia = *(const int *)a, ib = *(const int *)b;
  const int sa = s_score_scratch[ia], sb = s_score_scratch[ib];
  if (sa != sb)
    return sb - sa; // higher score first
  return ia - ib;   // stable: keep git/sort order on ties
}

/**
 * @brief Recomputes E.file_finder.match[] for the current query and
 * clamps `selected` into the new range.
 */
static void rebuildMatches(void) {
  FileFinder *ff = &E.file_finder;
  int i;
  int sc;

  if (!ff->match)
    ff->match =
        galloc((size_t)(ff->path_count > 0 ? ff->path_count : 1) * sizeof(int));
  s_score_scratch =
      galloc((size_t)(ff->path_count > 0 ? ff->path_count : 1) * sizeof(int));
  if (!ff->match || !s_score_scratch) {
    ff->match_count = 0;
    gfree(s_score_scratch);
    s_score_scratch = NULL;
    return;
  }

  ff->query[ff->query_len] = '\0';

  ff->match_count = 0;
  for (i = 0; i < ff->path_count; i++) {
    sc = fuzzyScore(ff->query, ff->paths[i]);
    if (sc < 0)
      continue;
    s_score_scratch[i] = sc;
    ff->match[ff->match_count++] = i;
  }

  if (ff->query_len > 0)
    qsort(ff->match, (size_t)ff->match_count, sizeof(int), compareByScore);
  gfree(s_score_scratch);
  s_score_scratch = NULL;

  if (ff->selected >= ff->match_count)
    ff->selected = ff->match_count > 0 ? ff->match_count - 1 : 0;
  if (ff->selected < 0)
    ff->selected = 0;
}

// ─── Open / close ──────────────────────────────────────────────────────────

void fileFinderClose(void) {
  FileFinder *ff = &E.file_finder;
  int i;
  for (i = 0; i < ff->path_count; i++)
    gfree(ff->paths[i]);
  gfree(ff->paths);
  gfree(ff->match);
  gfree(ff->root);
  ff->root = NULL;
  ff->paths = NULL;
  ff->match = NULL;
  ff->path_count = ff->match_count = 0;
  ff->query_len = 0;
  ff->query[0] = '\0';
  ff->selected = 0;
  ff->visible = 0;
}

void fileFinderOpen(void) { fileFinderOpenAt(NULL); }

void fileFinderOpenAt(const char *dir) {
  char *root;
  char *output;
  fileFinderClose(); // drop any stale snapshot first

  root = dir ? utilStrdup(dir) : projectRootOrCwd();
  if (!root) {
    editorSetStatusMessage("find-file: can't determine the working directory");
    return;
  }

  output = listProjectFiles(root);
  if (!output) {
    editorSetStatusMessage("find-file: no files found under %s", root);
    gfree(root);
    return;
  }
  ownPaths(output);
  gfree(output);

  if (E.file_finder.path_count == 0) {
    editorSetStatusMessage("find-file: no files found under %s", root);
    gfree(root);
    fileFinderClose();
    return;
  }
  E.file_finder.root = root; // fileFinderClose() frees it

  rebuildMatches();
  E.file_finder.visible = 1;
}

// ─── Opening the chosen file ───────────────────────────────────────────────

/**
 * @brief buffer_id of an already-open FILE_BUFF for `rel_path` (resolved
 * against the finder's root), or -1. Keeps picking the same file from
 * stacking up duplicate buffers.
 */
static int findOpenBuffer(const char *rel_path) {
  char want[PATH_MAX];
  int first;
  Buffer *b;
  char *full;
  int same;
  if (E.buffers == NULL)
    return -1;

  if (os_path_is_absolute(rel_path)) {
    snprintf(want, sizeof(want), "%s", rel_path);
  } else {
    if (!E.file_finder.root)
      return -1;
    snprintf(want, sizeof(want), "%s/%s", E.file_finder.root, rel_path);
  }

  first = E.buffers->buffer_id;
  b = E.buffers;
  do {
    if (b->type == FILE_BUFF && b->fullname) {
      full = (char *)utf8ToString(b->fullname);
      if (full) {
        same = os_path_equal(full, want);
        gfree(full);
        if (same)
          return b->buffer_id;
      }
    }
    b = b->next;
  } while (b->buffer_id != first);

  return -1;
}

static void openSelected(void) {
  const FileFinder *ff = &E.file_finder;
  const char *rel;
  EditorPane *active;
  int existing;
  char full[PATH_MAX];
  Utf8Char *path;
  int id;
  if (ff->selected < 0 || ff->selected >= ff->match_count)
    return;

  rel = ff->paths[ff->match[ff->selected]];
  active = splitScreenGetActivePane();

  existing = findOpenBuffer(rel);
  if (existing != -1) {
    active->buffer_id = existing;
  } else {
    // Relative to the finder's root, which is the project's and not
    // necessarily the working directory.
    if (ff->root && !os_path_is_absolute(rel))
      snprintf(full, sizeof(full), "%s/%s", ff->root, rel);
    else
      snprintf(full, sizeof(full), "%s", rel);
    path = stringToUtf8((unsigned char *)full);
    if (!path)
      return;
    id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
    gfree(path);
    if (id == -1) {
      editorSetStatusMessage("find-file: could not open %s", rel);
      return;
    }
    active->buffer_id = id;
  }

  active->cursor_x = active->cursor_y = 0;
  active->x_offset = active->y_offset = 0;
  editorSetStatusMessage("%s", rel);
}

// ─── Input ─────────────────────────────────────────────────────────────────

int fileFinderHandleKey(const Utf8Char *key) {
  FileFinder *ff = &E.file_finder;
  unsigned char c;
  int is_single;
  int had_match;
  int is_text;
  int n;
  if (!ff->visible)
    return 0;

  c = key->character[0];
  is_single = (key->length == 1);

  if (is_single && c == '\x1b') { // ESC — cancel
    fileFinderClose();
    return 1;
  }
  if (is_single && (c == ARROW_DOWN || c == CTRL_KEY('n'))) {
    if (ff->selected < ff->match_count - 1)
      ff->selected++;
    return 1;
  }
  if (is_single && (c == ARROW_UP || c == CTRL_KEY('p'))) {
    if (ff->selected > 0)
      ff->selected--;
    return 1;
  }
  if (is_single && c == '\r') { // ENTER — open the highlight
    had_match = ff->match_count > 0;
    openSelected();
    fileFinderClose();
    if (!had_match)
      editorSetStatusMessage("find-file: nothing selected");
    return 1;
  }
  if (is_single && (c == BACKSPACE || c == DEL_KEY)) {
    if (ff->query_len > 0) {
      // Step back over a whole UTF-8 char: drop trailing continuation
      // bytes (0b10xxxxxx) then the lead byte.
      ff->query_len--;
      while (ff->query_len > 0 && (ff->query[ff->query_len] & 0xC0) == 0x80)
        ff->query_len--;
      ff->query[ff->query_len] = '\0';
      ff->selected = 0;
      rebuildMatches();
    }
    return 1;
  }

  // Typed text — printable ASCII, or any multi-byte UTF-8 char. The
  // remapped special keys (arrows etc.) are all >= 0x80 with length 1,
  // so they're excluded here.
  is_text = (is_single && c >= 0x20 && c <= 0x7E) || key->length > 1;
  if (is_text) {
    n = (unsigned char) key->length;
    if (ff->query_len + n < (int)sizeof(ff->query) - 1) {
      memcpy(ff->query + ff->query_len, key->character, (size_t)n);
      ff->query_len += n;
      ff->query[ff->query_len] = '\0';
      ff->selected = 0;
      rebuildMatches();
    }
    return 1;
  }

  // Modal: swallow anything else so it doesn't fall through into the
  // buffer behind the popup.
  return 1;
}

// ─── Draw ──────────────────────────────────────────────────────────────────

void fileFinderDraw(struct abuf *ab) {
  FileFinder *ff = &E.file_finder;
  int visible_count;
  int scroll;
  char title[160];
  int content_w;
  int i;
  int len;
  int box_w;
  int rows;
  int box_h;
  int origin_x;
  int origin_y;
  PopupStyle style;
  char empty[LIST_POPUP_MAX_WIDTH + 1];
  int idx;
  int is_sel;
  int row;
  const char *path;
  char col[LIST_POPUP_MAX_WIDTH + 1];
  int plen;
  if (!ff->visible)
    return;

  visible_count = ff->match_count < FF_VISIBLE_ROWS ? ff->match_count : FF_VISIBLE_ROWS;

  scroll = 0;
  if (ff->selected >= FF_VISIBLE_ROWS)
    scroll = ff->selected - FF_VISIBLE_ROWS + 1;

  // Title row shows the live query and the match count.
  snprintf(title, sizeof(title), "Find file: %s_  (%d)", ff->query,
           ff->match_count);

  content_w = (int)strlen(title);
  for (i = 0; i < visible_count; i++) {
    len = (int)strlen(ff->paths[ff->match[scroll + i]]);
    if (len > content_w)
      content_w = len;
  }
  if (content_w > E.screencols - 6)
    content_w = E.screencols - 6;
  if (content_w > LIST_POPUP_MAX_WIDTH)
    content_w = LIST_POPUP_MAX_WIDTH;
  if (content_w < 20)
    content_w = 20;

  box_w = content_w + 4;
  // top border, title, separator, rows (at least one, so an empty result
  // set still draws a box rather than vanishing), bottom border
  rows = visible_count > 0 ? visible_count : 1;
  box_h = rows + 4;

  popupCenter(box_w, box_h, &origin_x, &origin_y);

  popupStyleInit(&style);

  popupDrawTop(ab, &style, origin_x, origin_y, box_w);
  popupDrawTitleRow(ab, &style, origin_x, origin_y + 1, box_w, title,
                    content_w);
  popupDrawSeparator(ab, &style, origin_x, origin_y + 2, box_w);

  if (visible_count == 0) {
    popupRowBegin(ab, &style, origin_x, origin_y + 3, 0);
    snprintf(empty, sizeof(empty), "%-*s", content_w, "  (no matches)");
    empty[content_w] = '\0';
    abAppend(ab, empty, content_w);
    popupRowEnd(ab, &style);
  }

  for (i = 0; i < visible_count; i++) {
    idx = scroll + i;
    is_sel = (idx == ff->selected);
    row = origin_y + 3 + i;

    popupRowBegin(ab, &style, origin_x, row, is_sel);

    path = ff->paths[ff->match[idx]];
    plen = (int)strlen(path);
    // Left-justify when it fits; when it doesn't, show the tail rather
    // than the head — the basename is what you're scanning for.
    snprintf(col, sizeof(col), "%-*s", content_w,
             plen <= content_w ? path : path + plen - content_w);
    col[content_w] = '\0';
    abAppend(ab, col, content_w);

    popupRowEnd(ab, &style);
  }

  popupDrawBottom(ab, &style, origin_x, origin_y + 3 + rows, box_w);
  popupDrawShadow(ab, &style, origin_x, origin_y, box_w, box_h);
}
