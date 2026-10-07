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
#include "../include/split_screen.h"
#include "../include/subprocess.h"
#include "../include/terminal.h"
#include "../include/utf8.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

extern EditorConfig E;

#define FF_LIST_CMD_TIMEOUT_MS 4000
#define FF_MAX_OUTPUT (8 * 1024 * 1024)
#define FF_MAX_FILES 50000
#define FF_VISIBLE_ROWS LIST_POPUP_MAX_H

// ─── Building the file list ─────────────────────────────────────────────────

/**
 * @brief Is `root` inside a git work tree? Same cheap check project
 * search makes — ask git rather than hunt for a .git dir, which gets
 * worktrees and submodules wrong.
 */
static int rootIsGitRepo(const char *root) {
  char qroot[PATH_MAX + 4];
  char cmd[PATH_MAX + 96];
  char *out;
  int yes;
#ifdef _WIN32
  char *argv[] = {"git", "-C", (char *)root, "rev-parse", "--is-inside-work-tree", NULL};
  (void)qroot;
  (void)cmd;
  out = runArgvCaptureQuiet(argv, 1000, 64, NULL);
#else
  shellQuote(root, qroot, sizeof(qroot));

  snprintf(cmd, sizeof(cmd),
           "git -C %s rev-parse --is-inside-work-tree 2>/dev/null", qroot);

  out = runCommandCapture(cmd, 1000, 64);
#endif
  if (!out)
    return 0;
  yes = (strncmp(out, "true", 4) == 0);
  gfree(out);
  return yes;
}

/**
 * @brief Runs the file-listing command and returns its raw newline-
 * separated stdout (paths relative to `root`), or NULL.
 * @details git branch: tracked files plus untracked-but-not-ignored ones
 * (so a file you just created shows up), de-duplicated. Non-git branch:
 * find(1) from the root with the usual heavy directories pruned. Both are
 * run with cwd = root so every path comes back root-relative.
 */
#ifdef _WIN32
// ─── Windows: the same lists, without sh, tr, sort or find ──────────────────

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
 * @brief Sorts the lines of `text` (separated by `sep`) byte-wise and drops
 * duplicates — `LC_ALL=C sort -u`. Returns a galloc'd,
 * newline-separated copy; consumes `text`.
 */
static char *sortUniqueLines(char *text, const char sep) {
  size_t count = 0;
  size_t cap = 1024;
  char **lines = galloc(cap * sizeof(char *));
  char **grown;
  char *p = text;
  char *end;
  PathList out = {NULL, 0, 0, 0};
  size_t i;

  if (!lines) {
    gfree(text);
    return NULL;
  }
  while (*p) {
    end = strchr(p, sep);
    if (end)
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
    if (!end)
      break;
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

/**
 * @brief Adds every file under root/rel to `list`, pruning the same
 * directories as the Unix find(1) branch.
 */
static void walkFiles(const char *root, const char *rel, PathList *list) {
  char dir_path[PATH_MAX];
  char child_rel[PATH_MAX];
  char child_path[PATH_MAX];
  struct stat st;
  struct dirent *entry;
  DIR *dir;
  const char *name;

  snprintf(dir_path, sizeof(dir_path), "%s%s%s", root, *rel ? "/" : "", rel);
  dir = opendir(dir_path);
  if (!dir)
    return;
  while (!list->full && (entry = readdir(dir)) != NULL) {
    name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel, *rel ? "/" : "", name);
    snprintf(child_path, sizeof(child_path), "%s/%s", root, child_rel);
    if (stat(child_path, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 ||
          strcmp(name, "build") == 0 || strcmp(name, ".beluga") == 0 ||
          strcmp(name, "dist") == 0)
        continue;
      walkFiles(root, child_rel, list);
    } else if (S_ISREG(st.st_mode)) {
      pathListAdd(list, child_rel);
    }
  }
  closedir(dir);
}

static char *listProjectFiles(const char *root) {
  // Newline-separated rather than -z: the captured output is a C string.
  // Safe here — git only quotes names containing '"', '\\' or control
  // characters, none of which Windows allows in a file name, and
  // core.quotePath=false keeps non-ASCII names as they are.
  char *argv[] = {"git", "-c", "core.quotePath=false", "-C", (char *)root, "ls-files",
                  "--cached", "--others", "--exclude-standard", NULL};
  PathList list = {NULL, 0, 0, 0};
  char *out;

  if (rootIsGitRepo(root)) {
    out = runArgvCaptureQuiet(argv, FF_LIST_CMD_TIMEOUT_MS, FF_MAX_OUTPUT, NULL);
    return out ? sortUniqueLines(out, '\n') : NULL;
  }
  walkFiles(root, "", &list);
  return list.text ? sortUniqueLines(list.text, '\n') : NULL;
}
#else
static char *listProjectFiles(const char *root) {
  char qroot[PATH_MAX + 4];
  char cmd[PATH_MAX * 2 + 512];
  shellQuote(root, qroot, sizeof(qroot));

  if (rootIsGitRepo(root)) {
    snprintf(cmd, sizeof(cmd),
             "cd %s && git ls-files --cached --others --exclude-standard "
             "-z 2>/dev/null | tr '\\0' '\\n' | LC_ALL=C sort -u",
             qroot);
  } else {
    snprintf(cmd, sizeof(cmd),
             "cd %s && find . \\( -name .git -o -name node_modules "
             "-o -name build -o -name .beluga -o -name dist \\) -prune "
             "-o -type f -print 2>/dev/null | LC_ALL=C sort",
             qroot);
  }

  return runCommandCapture(cmd, FF_LIST_CMD_TIMEOUT_MS, FF_MAX_OUTPUT);
}
#endif

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
  for (line = strtok_r(output, "\n", &saveptr);
       line && ff->path_count < FF_MAX_FILES;
       line = strtok_r(NULL, "\n", &saveptr)) {
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
    ff->paths[ff->path_count] = strdup(line);
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

static int *s_score_scratch = NULL; // score per path, indexed like paths[]

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
  if (!s_score_scratch)
    s_score_scratch =
        galloc((size_t)(ff->path_count > 0 ? ff->path_count : 1) * sizeof(int));
  if (!ff->match || !s_score_scratch) {
    ff->match_count = 0;
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
  gfree(s_score_scratch);
  ff->paths = NULL;
  ff->match = NULL;
  s_score_scratch = NULL;
  ff->path_count = ff->match_count = 0;
  ff->query_len = 0;
  ff->query[0] = '\0';
  ff->selected = 0;
  ff->visible = 0;
}

void fileFinderOpen(void) {
  char root[PATH_MAX];
  char *output;
  fileFinderClose(); // drop any stale snapshot first

  if (!getcwd(root, sizeof(root))) {
    editorSetStatusMessage("find-file: can't determine the working directory");
    return;
  }

  output = listProjectFiles(root);
  if (!output) {
    editorSetStatusMessage("find-file: no files found under %s", root);
    return;
  }
  ownPaths(output);
  gfree(output);

  if (E.file_finder.path_count == 0) {
    editorSetStatusMessage("find-file: no files found under %s", root);
    fileFinderClose();
    return;
  }

  rebuildMatches();
  E.file_finder.visible = 1;
}

// ─── Opening the chosen file ───────────────────────────────────────────────

/**
 * @brief buffer_id of an already-open FILE_BUFF for `rel_path` (resolved
 * against the editor's cwd), or -1. Keeps picking the same file from
 * stacking up duplicate buffers.
 */
static int findOpenBuffer(const char *rel_path) {
  char want[PATH_MAX];
  char cwd[PATH_MAX];
  int first;
  Buffer *b;
  char *full;
  int same;
  if (E.buffers == NULL)
    return -1;

  if (IS_ABS_PATH(rel_path)) {
    snprintf(want, sizeof(want), "%s", rel_path);
  } else {
    if (!getcwd(cwd, sizeof(cwd)))
      return -1;
    snprintf(want, sizeof(want), "%s/%s", cwd, rel_path);
  }

  first = E.buffers->buffer_id;
  b = E.buffers;
  do {
    if (b->type == FILE_BUFF && b->fullname) {
      full = (char *)utf8ToString(b->fullname);
      if (full) {
        same = (strcmp(full, want) == 0);
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
    path = stringToUtf8((unsigned char *)rel);
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
