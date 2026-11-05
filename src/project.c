/**
 * @file project.c
 * @brief Projects: finding a file's project root, remembering projects,
 * and each project's own .beluga/init.lisp. See project.h.
 */

#include "../include/project.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/file_finder.h"
#include "../include/galloc.h"
#include "../include/input.h"
#include "../include/settings.h"
#include "../include/list_popup.h"
#include "../include/os.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

// ~/.beluga/projects keeps this many, most recently used first.
#define PROJECTS_REMEMBERED 100

// A directory holding any of these is a project root. The nearest one
// above a file wins. (project-add-marker "Cargo.toml") adds to the list.
static const char *const s_default_markers[] = {PROJECT_CONFIG_DIR, ".git",
                                                "compile_commands.json", NULL};
static char **s_markers = NULL; // added ones, owned
static int s_marker_count = 0;

static long s_clock = 0;        // last_used stamps
static int s_loaded = 0;        // projectInit() has read ~/.beluga/projects
static int s_last_buffer_id = -1; // what projectSyncActive() last saw
static char s_notice[256];      // shown on the next tick, see setNotice()

// ─── Paths ─────────────────────────────────────────────────────────────────

/**
 * @brief `path` as an absolute, canonical directory with '/' separators
 * and no trailing slash, in `out`. Falls back to the path itself (made
 * absolute against the working directory) when it doesn't exist.
 */
static void canonicalDir(const char *path, char *out, size_t out_size) {
  char *real = os_realpath(path);
  char *cwd;
  size_t len;
  if (real) {
    snprintf(out, out_size, "%s", real);
    gfree(real);
  } else if (os_path_is_absolute(path)) {
    snprintf(out, out_size, "%s", path);
  } else {
    cwd = editorGetCwd();
    snprintf(out, out_size, "%s/%s", cwd ? cwd : "", path);
    gfree(cwd);
  }
  os_path_to_slashes(out);
  len = strlen(out);
  while (len > (size_t)os_path_drive_length(out) + 1 && out[len - 1] == '/')
    out[--len] = '\0';
}

static const char *baseName(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash && slash[1] ? slash + 1 : path;
}

static int findProject(const char *root);

static int hasMarker(const char *dir) {
  char path[OS_PATH_MAX];
  OsFileInfo info;
  int i;
  for (i = 0; s_default_markers[i]; i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, s_default_markers[i]);
    if (os_file_info(path, &info) == 0)
      return 1;
  }
  for (i = 0; i < s_marker_count; i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, s_markers[i]);
    if (os_file_info(path, &info) == 0)
      return 1;
  }
  return 0;
}

/**
 * @brief Whether `dir` is a remembered project: one project-add added
 * stays a root without any marker, and so does one whose .git went away.
 */
static int isRemembered(const char *dir) {
  const int index = findProject(dir);
  return index != -1 && !E.projects[index].removed;
}

/**
 * @brief The project root above `start_dir` (a canonical directory), in
 * `out`. 0 when there is none.
 * @details Never the home directory or anything above it, nor the
 * filesystem root: a dotfiles repository in ~ would otherwise make every
 * file you own one project.
 */
static int findRoot(const char *start_dir, char *out, size_t out_size) {
  char dir[OS_PATH_MAX];
  char parent[OS_PATH_MAX];
  char home[OS_PATH_MAX];
  char *home_dir = os_home_dir();
  if (!home_dir)
    return 0;
  canonicalDir(home_dir, home, sizeof(home));
  gfree(home_dir);
  snprintf(dir, sizeof(dir), "%s", start_dir);
  for (;;) {
    if (os_path_equal(dir, home))
      return 0;
    utilDirname(dir, parent, sizeof(parent));
    if (os_path_equal(parent, dir) || strcmp(parent, ".") == 0)
      return 0; // dir is the filesystem root
    if (hasMarker(dir) || isRemembered(dir)) {
      snprintf(out, out_size, "%s", dir);
      return 1;
    }
    snprintf(dir, sizeof(dir), "%s", parent);
  }
}

// ─── The project list ──────────────────────────────────────────────────────

static int findProject(const char *root) {
  int i;
  for (i = 0; i < E.number_of_projects; i++)
    if (os_path_equal(E.projects[i].root, root))
      return i;
  return -1;
}

/**
 * @brief Index of `root` in E.projects, adding it if it's new (or
 * bringing back one project-remove forgot); -1 on OOM.
 */
static int addProject(const char *root, int trusted, long last_used) {
  Project *grown;
  Project *p;
  int index = findProject(root);
  if (index != -1) {
    if (E.projects[index].removed) {
      E.projects[index].removed = 0;
      E.projects[index].last_used = last_used;
    }
    return index;
  }
  grown = (Project *)grealloc(E.projects,
                              (E.number_of_projects + 1) * sizeof(Project));
  if (!grown)
    return -1;
  E.projects = grown;
  p = &E.projects[E.number_of_projects];
  memset(p, 0, sizeof(*p));
  p->root = utilStrdup(root);
  if (!p->root)
    return -1;
  p->trusted = trusted;
  p->last_used = last_used;
  p->constantes = E.global_constantes;
  return E.number_of_projects++;
}

static void projectsFilePath(char *out, size_t out_size) {
  char *home = os_home_dir();
  snprintf(out, out_size, "%s/.beluga/projects", home ? home : ".");
  gfree(home);
}

/**
 * @brief Reads ~/.beluga/projects: one project per line, most recently
 * used first, as "+ /root" when trusted and "- /root" otherwise.
 */
static void loadProjects(void) {
  char path[OS_PATH_MAX];
  char line[OS_PATH_MAX + 4];
  FILE *f;
  size_t len;
  long n = PROJECTS_REMEMBERED;
  projectsFilePath(path, sizeof(path));
  f = fopen(path, "r");
  if (!f)
    return;
  while (fgets(line, sizeof(line), f) && n > 0) {
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len < 3 || (line[0] != '+' && line[0] != '-') || line[1] != ' ')
      continue;
    addProject(line + 2, line[0] == '+', n--);
  }
  fclose(f);
  s_clock = PROJECTS_REMEMBERED;
}

static int byLastUsed(const void *a, const void *b) {
  const Project *pa = *(const Project *const *)a;
  const Project *pb = *(const Project *const *)b;
  return pa->last_used < pb->last_used ? 1 : pa->last_used > pb->last_used ? -1 : 0;
}

/**
 * @brief The projects not removed, most recently used first, and their
 * number in `count` (galloc'd, NULL if none; `count` -1 on OOM).
 */
static Project **sortedProjects(int *count) {
  Project **sorted;
  int i;
  *count = 0;
  if (E.number_of_projects == 0)
    return NULL;
  sorted = (Project **)galloc(E.number_of_projects * sizeof(Project *));
  if (!sorted) {
    *count = -1;
    return NULL;
  }
  for (i = 0; i < E.number_of_projects; i++)
    if (!E.projects[i].removed)
      sorted[(*count)++] = &E.projects[i];
  if (*count == 0) {
    gfree(sorted);
    return NULL;
  }
  qsort(sorted, *count, sizeof(Project *), byLastUsed);
  return sorted;
}

static void saveProjects(void) {
  char path[OS_PATH_MAX];
  int count;
  Project **sorted = sortedProjects(&count);
  FILE *f;
  int i;
  // Not before projectInit() read the file, and not on OOM: either would
  // write over the remembered projects with nothing.
  if (!s_loaded || count < 0)
    return;
  projectsFilePath(path, sizeof(path));
  f = fopen(path, "w");
  if (f) {
    // Written even when empty, so removing the last project sticks.
    for (i = 0; i < count && i < PROJECTS_REMEMBERED; i++)
      fprintf(f, "%c %s\n", sorted[i]->trusted ? '+' : '-', sorted[i]->root);
    fclose(f);
  }
  gfree(sorted);
}

// ─── A project's own config ───────────────────────────────────────────────

/**
 * @brief Shows `fmt` in the status bar on the next tick rather than now:
 * at startup main() writes its own help line over anything said while
 * the first buffers open.
 */
static void setNotice(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_notice, sizeof(s_notice), fmt, ap);
  va_end(ap);
}

static void configPath(const Project *p, char *out, size_t out_size) {
  snprintf(out, out_size, "%s/%s/%s", p->root, PROJECT_CONFIG_DIR,
           PROJECT_CONFIG_FILE);
}

/**
 * @brief Evaluates the project's .beluga/init.lisp, if it has one and is
 * trusted, and reads its settings.
 * @details In a fresh environment on top of the global one, holding
 * PROJECT-ROOT: what it defines shadows init.lisp only for this project.
 * Builtins still act on the editor as a whole, so a map-key or add-hook
 * in it applies everywhere. Run again (project-trust) to reload it.
 */
static void loadConfig(Project *p) {
  char path[OS_PATH_MAX];
  OsFileInfo info;
  LispError error = LISP_ERROR_NONE;
  Lisp env;
  Lisp program;
  configPath(p, path, sizeof(path));
  p->has_config_env = 0;
  p->constantes = E.global_constantes;
  if (os_file_info(path, &info) != 0 || info.is_dir)
    return;
  if (!p->trusted) {
    setNotice("%s has a project config: project-trust (CTRL-x p t) loads it",
              baseName(p->root));
    return;
  }

  env = lisp_env_extend(lisp_env(E.ctx), lisp_make_table(E.ctx), E.ctx);
  lisp_env_define(env, lisp_make_symbol("PROJECT-ROOT", E.ctx),
                  lisp_make_string2(p->root, E.ctx), E.ctx);
  program = lisp_read_path(path, &error, E.ctx);
  if (error == LISP_ERROR_NONE)
    lisp_eval2(program, env, &error, E.ctx);
  if (error != LISP_ERROR_NONE)
    setNotice("%s: %s", path, lisp_error_string(error));

  p->config_env = env;
  p->has_config_env = 1;
  settingsRead(env, &p->constantes, NULL, 0);
}

/** @brief Makes E.projects[index] the current project. */
static void activate(int index) {
  Project *p;
  Lisp hook_args;
  if (index < 0 || index >= E.number_of_projects)
    return;
  p = &E.projects[index];
  if (p->removed) {
    // A buffer opened before project-remove forgot its project: it has
    // none now, and init.lisp's settings apply.
    E.current_project = -1;
    E.constantes = E.global_constantes;
    return;
  }
  p->last_used = ++s_clock;
  if (index == E.current_project)
    return;
  E.current_project = index;
  if (!p->opened) {
    p->opened = 1;
    loadConfig(p);
    saveProjects();
    hook_args = lisp_cons(lisp_make_string2(p->root, E.ctx), lisp_null(), E.ctx);
    runHooks("project-open", hook_args);
  }
  E.constantes = p->constantes;
}

// ─── Language servers ─────────────────────────────────────────────────────
// What you answered the first time one of a project's files wanted its
// language server, so later sessions don't ask again. Kept in
// ~/.beluga/lsp-roots, one per line: "+ server<TAB>project<TAB>root" to
// start it at root, "- server<TAB>project" not to start it.

typedef struct {
  char *server;   // the lsp-server command, argv[0]
  char *project;  // the project's root
  char *lsp_root; // where the server starts; NULL: declined
} LspChoice;

static LspChoice *s_lsp_choices = NULL;
static int s_lsp_choice_count = 0;

static void lspChoicesPath(char *out, size_t out_size) {
  char *home = os_home_dir();
  snprintf(out, out_size, "%s/.beluga/lsp-roots", home ? home : ".");
  gfree(home);
}

static int findLspChoice(const char *project, const char *server) {
  int i;
  for (i = 0; i < s_lsp_choice_count; i++)
    if (strcmp(s_lsp_choices[i].server, server) == 0 &&
        os_path_equal(s_lsp_choices[i].project, project))
      return i;
  return -1;
}

static void saveLspChoices(void) {
  char path[OS_PATH_MAX];
  const LspChoice *c;
  FILE *f;
  int i;
  if (!s_loaded)
    return; // as saveProjects(): not over a file not read yet
  lspChoicesPath(path, sizeof(path));
  f = fopen(path, "w");
  if (!f)
    return;
  for (i = 0; i < s_lsp_choice_count; i++) {
    c = &s_lsp_choices[i];
    if (c->lsp_root)
      fprintf(f, "+ %s\t%s\t%s\n", c->server, c->project, c->lsp_root);
    else
      fprintf(f, "- %s\t%s\n", c->server, c->project);
  }
  fclose(f);
}

/**
 * @brief Records that `server` starts at `lsp_root` (NULL: doesn't start)
 * in `project`, replacing what was there. 0 on OOM.
 */
static int setLspChoice(const char *project, const char *server,
                        const char *lsp_root) {
  LspChoice *grown;
  LspChoice *c;
  char *root_copy = NULL;
  int index = findLspChoice(project, server);
  if (lsp_root) {
    root_copy = utilStrdup(lsp_root);
    if (!root_copy)
      return 0;
  }
  if (index != -1) {
    c = &s_lsp_choices[index];
    gfree(c->lsp_root);
    c->lsp_root = root_copy;
    return 1;
  }
  grown = (LspChoice *)grealloc(s_lsp_choices,
                                (s_lsp_choice_count + 1) * sizeof(LspChoice));
  if (!grown) {
    gfree(root_copy);
    return 0;
  }
  s_lsp_choices = grown;
  c = &s_lsp_choices[s_lsp_choice_count];
  c->server = utilStrdup(server);
  c->project = utilStrdup(project);
  c->lsp_root = root_copy;
  if (!c->server || !c->project) {
    gfree(c->server);
    gfree(c->project);
    gfree(c->lsp_root);
    return 0;
  }
  s_lsp_choice_count++;
  return 1;
}

static void loadLspChoices(void) {
  char path[OS_PATH_MAX];
  char line[2 * OS_PATH_MAX + 300];
  char *server;
  char *project;
  char *lsp_root;
  char *tab;
  FILE *f;
  size_t len;
  lspChoicesPath(path, sizeof(path));
  f = fopen(path, "r");
  if (!f)
    return;
  while (fgets(line, sizeof(line), f)) {
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len < 3 || (line[0] != '+' && line[0] != '-') || line[1] != ' ')
      continue;
    server = line + 2;
    tab = strchr(server, '\t');
    if (!tab)
      continue;
    *tab = '\0';
    project = tab + 1;
    lsp_root = NULL;
    tab = strchr(project, '\t');
    if (tab) {
      *tab = '\0';
      lsp_root = tab + 1;
    }
    if (line[0] == '+' && (!lsp_root || !lsp_root[0]))
      continue;
    setLspChoice(project, server, line[0] == '+' ? lsp_root : NULL);
  }
  fclose(f);
}

/** @brief Forgets every choice made in `project`; returns how many. */
static int forgetLspChoices(const char *project) {
  int i;
  int kept = 0;
  int forgotten;
  for (i = 0; i < s_lsp_choice_count; i++) {
    if (os_path_equal(s_lsp_choices[i].project, project)) {
      gfree(s_lsp_choices[i].server);
      gfree(s_lsp_choices[i].project);
      gfree(s_lsp_choices[i].lsp_root);
    } else {
      s_lsp_choices[kept++] = s_lsp_choices[i];
    }
  }
  forgotten = s_lsp_choice_count - kept;
  s_lsp_choice_count = kept;
  if (forgotten)
    saveLspChoices();
  return forgotten;
}

static void freeLspChoices(void) {
  int i;
  for (i = 0; i < s_lsp_choice_count; i++) {
    gfree(s_lsp_choices[i].server);
    gfree(s_lsp_choices[i].project);
    gfree(s_lsp_choices[i].lsp_root);
  }
  gfree(s_lsp_choices);
  s_lsp_choices = NULL;
  s_lsp_choice_count = 0;
}

static int isDir(const char *path) {
  OsFileInfo info;
  return os_file_info(path, &info) == 0 && info.is_dir;
}

/**
 * @brief Drops the project's own config: its settings give way to
 * init.lisp's. What its init.lisp did to the editor as a whole (map-key,
 * add-hook) stays until beluga restarts.
 */
static void unloadConfig(Project *p) {
  p->has_config_env = 0;
  p->constantes = E.global_constantes;
  if (E.current_project >= 0 && p == &E.projects[E.current_project])
    E.constantes = E.global_constantes;
}

/**
 * @brief Forgets E.projects[index]: it leaves the list and
 * ~/.beluga/projects, untrusted. Nothing on disk is touched, and opening
 * a file in it again brings it back like a new one.
 * @details Marked rather than taken out of E.projects, since buffers and
 * daemon windows hold indexes into it.
 */
static void removeProject(int index) {
  Project *p = &E.projects[index];
  unloadConfig(p);
  forgetLspChoices(p->root);
  p->removed = 1;
  p->trusted = 0;
  p->opened = 0;
  if (index == E.current_project)
    E.current_project = -1;
}

/** @brief The project above directory `dir`, added if new; -1 if none. */
static int projectForDir(const char *dir) {
  char canonical[OS_PATH_MAX];
  char root[OS_PATH_MAX];
  canonicalDir(dir, canonical, sizeof(canonical));
  if (!findRoot(canonical, root, sizeof(root)))
    return -1;
  return addProject(root, 0, s_clock);
}

// ─── project.h ─────────────────────────────────────────────────────────────

void projectInit(void) {
  char *cwd;
  int index;
  loadProjects();
  loadLspChoices();
  s_loaded = 1;
  // The working directory's project is current until a buffer says
  // otherwise, so (beluga with no file) inside a project still gets it.
  cwd = os_getcwd();
  if (cwd) {
    index = projectForDir(cwd);
    gfree(cwd);
    activate(index);
  }
}

void projectFree(void) {
  int i;
  saveProjects();
  freeLspChoices();
  for (i = 0; i < E.number_of_projects; i++)
    gfree(E.projects[i].root);
  gfree(E.projects);
  E.projects = NULL;
  E.number_of_projects = 0;
  E.current_project = -1;
  s_loaded = 0;
  for (i = 0; i < s_marker_count; i++)
    gfree(s_markers[i]);
  gfree(s_markers);
  s_markers = NULL;
  s_marker_count = 0;
}

void projectActivateForBuffer(Buffer *buffer) {
  char *dir;
  int index;
  if (!buffer)
    return;
  if (buffer->project == 0) {
    buffer->project = -1;
    if (buffer->type == FILE_BUFF && buffer->path) {
      dir = (char *)utf8ToString(buffer->path);
      if (dir) {
        index = projectForDir(dir);
        gfree(dir);
        if (index != -1)
          buffer->project = index + 1;
      }
    }
  }
  if (buffer->project > 0)
    activate(buffer->project - 1);
}

void projectSwapLastBuffer(int *other) {
  const int mine = s_last_buffer_id;
  s_last_buffer_id = *other;
  *other = mine;
}

void projectSyncActive(void) {
  const EditorPane *active = splitScreenGetActivePane();
  if (active && active->buffer_id != s_last_buffer_id) {
    // Only when the active buffer changes, so a project picked with
    // project-switch stays current until you move to another buffer.
    s_last_buffer_id = active->buffer_id;
    projectActivateForBuffer(bufferFindById(active->buffer_id));
  }
  if (s_notice[0]) {
    editorSetStatusMessage("%s", s_notice);
    s_notice[0] = '\0';
  }
}

const char *projectCurrentRoot(void) {
  if (E.current_project < 0 || E.current_project >= E.number_of_projects)
    return NULL;
  return E.projects[E.current_project].root;
}

char *projectRootOrCwd(void) {
  const char *root = projectCurrentRoot();
  char *cwd;
  char *copy;
  if (root)
    return utilStrdup(root);
  cwd = editorGetCwd();
  if (!cwd)
    return NULL;
  copy = utilStrdup(cwd);
  gfree(cwd);
  return copy;
}

int projectLspRoot(Buffer *buffer, const char *server, char *out,
                   size_t out_size) {
  Project *p;
  Lisp val;
  int present;
  int index;
  const char *dir;
  out[0] = '\0';
  // Makes the buffer's project current, so its config is loaded already.
  projectActivateForBuffer(buffer);
  if (!buffer || buffer->project <= 0)
    return -1;
  p = &E.projects[buffer->project - 1];
  if (p->removed)
    return -1;
  if (p->has_config_env) {
    val = lisp_env_lookup(p->config_env, lisp_make_symbol("LSP-ROOT", E.ctx),
                          &present);
    if (present && lisp_type(val) == LISP_STRING) {
      dir = lisp_string(val);
      if (os_path_is_absolute(dir) || !dir[0])
        snprintf(out, out_size, "%s", dir[0] ? dir : p->root);
      else
        snprintf(out, out_size, "%s/%s", p->root, dir);
      if (isDir(out))
        return 1;
      setNotice("LSP-ROOT %s isn't a directory", out);
    } else if (present && !lisp_is_true(val)) {
      return 0;
    }
  }
  index = findLspChoice(p->root, server);
  if (index != -1) {
    if (!s_lsp_choices[index].lsp_root)
      return 0;
    snprintf(out, out_size, "%s", s_lsp_choices[index].lsp_root);
    if (isDir(out))
      return 1; // moved or deleted since: ask again, suggesting the project
  }
  snprintf(out, out_size, "%s", p->root);
  return -1;
}

void projectLspRemember(Buffer *buffer, const char *server,
                        const char *lsp_root) {
  char canonical[OS_PATH_MAX];
  if (!buffer || buffer->project <= 0 ||
      E.projects[buffer->project - 1].removed)
    return;
  if (lsp_root)
    canonicalDir(lsp_root, canonical, sizeof(canonical));
  if (setLspChoice(E.projects[buffer->project - 1].root, server,
                   lsp_root ? canonical : NULL))
    saveLspChoices();
}

// ─── Builtins ──────────────────────────────────────────────────────────────

/**
 * @brief (project-root) — the current project's root, or "" without one.
 * @details The current project follows the active buffer (see project.h),
 * falling back to the working directory's at startup.
 */
static Lisp l_projectRoot(Lisp args, LispError *e, LispContext ctx) {
  const char *root = projectCurrentRoot();
  (void)args;
  (void)e;
  return lisp_make_string2(root ? root : "", ctx);
}

/** @brief (project-name) — the current project root's last component, or "". */
static Lisp l_projectName(Lisp args, LispError *e, LispContext ctx) {
  const char *root = projectCurrentRoot();
  (void)args;
  (void)e;
  return lisp_make_string2(root ? baseName(root) : "", ctx);
}

/** @brief (project-list) — every remembered project root, most recent first. */
static Lisp l_projectList(Lisp args, LispError *e, LispContext ctx) {
  int count;
  Project **sorted = sortedProjects(&count);
  Lisp list = lisp_null();
  int i;
  (void)args;
  (void)e;
  if (!sorted)
    return list;
  for (i = count - 1; i >= 0; i--)
    list = lisp_cons(lisp_make_string2(sorted[i]->root, ctx), list, ctx);
  gfree(sorted);
  return list;
}

/**
 * @brief (project-add-marker "name") — a file or directory called `name`
 * also marks a project root from now on (Cargo.toml, go.mod, ...).
 */
static Lisp l_projectAddMarker(Lisp args, LispError *e, LispContext ctx) {
  char **grown;
  char *copy;
  (void)e;
  (void)ctx;
  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING)
    return lisp_null();
  copy = utilStrdup(lisp_string(lisp_car(args)));
  grown = (char **)grealloc(s_markers, (s_marker_count + 1) * sizeof(char *));
  if (!copy || !grown) {
    gfree(copy);
    if (grown)
      s_markers = grown;
    return lisp_null();
  }
  s_markers = grown;
  s_markers[s_marker_count++] = copy;
  return lisp_null();
}

/**
 * @brief (project-trust) — trusts the current project, remembers it, and
 * (re)loads its .beluga/init.lisp.
 */
static Lisp l_projectTrust(Lisp args, LispError *e, LispContext ctx) {
  Project *p;
  char path[OS_PATH_MAX];
  OsFileInfo info;
  (void)args;
  (void)e;
  (void)ctx;
  if (E.current_project < 0) {
    editorSetStatusMessage("Not in a project");
    return lisp_null();
  }
  p = &E.projects[E.current_project];
  p->trusted = 1;
  saveProjects();
  loadConfig(p);
  E.constantes = p->constantes;
  configPath(p, path, sizeof(path));
  if (!s_notice[0])
    setNotice(os_file_info(path, &info) == 0
                  ? "%s trusted, project config loaded"
                  : "%s trusted (no .beluga/init.lisp yet)",
              baseName(p->root));
  return lisp_null();
}

/**
 * @brief Writes a new .beluga/init.lisp at `path`: a short header and
 * every setting as a commented-out placeholder. 0 if it can't be written.
 */
static int writeConfigTemplate(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return 0;
  fprintf(f,
          ";; Project config: evaluated when one of this project's files is\n"
          ";; opened, once the project is trusted (project-trust, CTRL-x p t).\n"
          ";; A (define ...) here only applies while one of its buffers is\n"
          ";; active; PROJECT-ROOT holds the project's root. map-key and\n"
          ";; add-hook still act on the whole editor.\n"
          ";;\n"
          ";; Uncomment a setting to override it for this project. Each shows\n"
          ";; the value your init.lisp gives it. Save, then CTRL-x p t reloads.\n"
          ";; (settings) lists the values in effect.\n\n"
          ";; Where this project's language servers start, relative to\n"
          ";; PROJECT-ROOT, without asking; #f never starts them. Otherwise\n"
          ";; you're asked once and the answer is remembered\n"
          ";; (project-lsp-forget, CTRL-x p L, asks again).\n"
          ";; (define LSP-ROOT \"\")\n\n");
  settingsWriteTemplate(f);
  return fclose(f) == 0;
}

/**
 * @brief (project-open-config) — opens the current project's
 * .beluga/init.lisp, creating its .beluga directory if needed.
 * @details A config created this way is yours, so the project becomes
 * trusted; opening one that already exists doesn't trust it. A new one
 * starts with every setting as a commented-out placeholder.
 */
static Lisp l_projectOpenConfig(Lisp args, LispError *e, LispContext ctx) {
  Project *p;
  char path[OS_PATH_MAX];
  char err[256];
  OsFileInfo info;
  Utf8Char *upath;
  EditorPane *active;
  (void)args;
  (void)e;
  (void)ctx;
  if (E.current_project < 0) {
    editorSetStatusMessage("Not in a project");
    return lisp_null();
  }
  p = &E.projects[E.current_project];
  configPath(p, path, sizeof(path));
  if (os_file_info(path, &info) != 0) {
    snprintf(path, sizeof(path), "%s/%s", p->root, PROJECT_CONFIG_DIR);
    if (os_mkdirs(path, err, sizeof(err)) != 0) {
      editorSetStatusMessage("project-open-config: %s", err);
      return lisp_null();
    }
    p->trusted = 1;
    saveProjects();
    configPath(p, path, sizeof(path));
    if (!writeConfigTemplate(path)) {
      editorSetStatusMessage("project-open-config: can't write %s", path);
      return lisp_null();
    }
  }
  upath = stringToUtf8((unsigned char *)path);
  if (!upath)
    return lisp_null();
  active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(upath, READ_AND_WRITE, FILE_BUFF);
  gfree(upath);
  return lisp_null();
}

/**
 * @brief Lists the remembered projects, most recent first (so the current
 * one is on top), as "name  /root"; choosing one calls `on_select` with
 * its root.
 */
static void openProjectPicker(const char *title, LispCFunc on_select,
                              LispContext ctx) {
  int count;
  Project **sorted = sortedProjects(&count);
  char label[OS_PATH_MAX + 64];
  Lisp items = lisp_null();
  int i;
  if (!sorted) {
    editorSetStatusMessage("No projects yet: open a file inside one, or project-add");
    return;
  }
  for (i = count - 1; i >= 0; i--) {
    snprintf(label, sizeof(label), "%-20s %s%s", baseName(sorted[i]->root),
             sorted[i]->root, sorted[i]->trusted ? "  (trusted)" : "");
    items = lisp_cons(lisp_cons(lisp_make_string2(label, ctx),
                                lisp_make_string2(sorted[i]->root, ctx), ctx),
                      items, ctx);
  }
  gfree(sorted);
  listPopupOpen(title, items, lisp_make_func(on_select), lisp_null(), 1);
}

/**
 * @brief (project-untrust) — stops trusting the current project: its
 * .beluga/init.lisp no longer loads, and its settings give way to
 * init.lisp's.
 */
static Lisp l_projectUntrust(Lisp args, LispError *e, LispContext ctx) {
  Project *p;
  int had_config;
  (void)args;
  (void)e;
  (void)ctx;
  if (E.current_project < 0) {
    editorSetStatusMessage("Not in a project");
    return lisp_null();
  }
  p = &E.projects[E.current_project];
  if (!p->trusted) {
    editorSetStatusMessage("%s isn't trusted", baseName(p->root));
    return lisp_null();
  }
  had_config = p->has_config_env;
  p->trusted = 0;
  saveProjects();
  unloadConfig(p);
  editorSetStatusMessage(had_config ? "%s untrusted, its config unloaded (key maps and hooks it set stay until restart)"
                                    : "%s untrusted",
                         baseName(p->root));
  return lisp_null();
}

/** @brief Forgets the project at `root`, saying so. 0 if it isn't one. */
static int removeRoot(const char *root) {
  char canonical[OS_PATH_MAX];
  int index;
  canonicalDir(root, canonical, sizeof(canonical));
  index = findProject(canonical);
  if (index == -1 || E.projects[index].removed) {
    editorSetStatusMessage("Not a remembered project: %s", canonical);
    return 0;
  }
  removeProject(index);
  saveProjects();
  editorSetStatusMessage("Forgot project %s (its files are untouched)",
                         E.projects[index].root);
  return 1;
}

/** @brief project-remove's on-select. */
static Lisp projectRemoveSelected(Lisp args, LispError *e, LispContext ctx) {
  (void)e;
  (void)ctx;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING)
    removeRoot(lisp_string(lisp_car(args)));
  return lisp_null();
}

/**
 * @brief (project-remove) or (project-remove "/root") — forgets a
 * project: it leaves project-switch and ~/.beluga/projects, and is no
 * longer trusted. Without an argument, picks it from the list (the
 * current one on top). Its files, .beluga included, are left alone.
 */
static Lisp l_projectRemove(Lisp args, LispError *e, LispContext ctx) {
  (void)e;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING)
    return lisp_make_bool(removeRoot(lisp_string(lisp_car(args))));
  openProjectPicker("Remove project", projectRemoveSelected, ctx);
  return lisp_null();
}

/**
 * @brief (project-prune) — forgets every remembered project whose
 * directory no longer exists; returns how many.
 */
static Lisp l_projectPrune(Lisp args, LispError *e, LispContext ctx) {
  OsFileInfo info;
  int i;
  int pruned = 0;
  (void)args;
  (void)e;
  (void)ctx;
  for (i = 0; i < E.number_of_projects; i++) {
    if (E.projects[i].removed)
      continue;
    if (os_file_info(E.projects[i].root, &info) != 0 || !info.is_dir) {
      removeProject(i);
      pruned++;
    }
  }
  if (pruned)
    saveProjects();
  editorSetStatusMessage(pruned == 1 ? "Forgot %d project that no longer exists"
                                     : "Forgot %d projects that no longer exist",
                         pruned);
  return lisp_make_int(pruned);
}

/**
 * @brief (project-add) or (project-add "dir") — remembers `dir` as a
 * project and makes it current, even without a marker: from then on it
 * is the root for the files under it. Without an argument, prompts for
 * the directory, starting from the working directory.
 */
static Lisp l_projectAdd(Lisp args, LispError *e, LispContext ctx) {
  char dir[OS_PATH_MAX];
  char canonical[OS_PATH_MAX];
  char *cwd;
  char *answer;
  Utf8Char *placeholder;
  Utf8Char *input;
  OsFileInfo info;
  int index;
  (void)e;
  (void)ctx;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING) {
    snprintf(dir, sizeof(dir), "%s", lisp_string(lisp_car(args)));
  } else {
    cwd = editorGetCwd();
    snprintf(dir, sizeof(dir), "%s/", cwd ? cwd : "");
    gfree(cwd);
    os_path_to_slashes(dir);
    placeholder = stringToUtf8((unsigned char *)dir);
    input = editorPrompt("Add project : %u", placeholder, 1);
    gfree(placeholder);
    if (!input)
      return lisp_null(); // cancelled
    answer = (char *)utf8ToString(input);
    gfree(input);
    if (!answer)
      return lisp_null();
    snprintf(dir, sizeof(dir), "%s", answer);
    gfree(answer);
  }
  canonicalDir(dir, canonical, sizeof(canonical));
  if (os_file_info(canonical, &info) != 0 || !info.is_dir) {
    editorSetStatusMessage("project-add: %s isn't a directory", canonical);
    return lisp_null();
  }
  index = addProject(canonical, 0, ++s_clock);
  if (index == -1)
    return lisp_null();
  activate(index);
  saveProjects();
  if (!s_notice[0])
    setNotice("Project %s added", baseName(canonical));
  return lisp_make_string2(canonical, ctx);
}

/**
 * @brief (project-lsp-forget) — forgets the current project's answers to
 * "start the language server?" and where: the next file opened asks
 * again. A server already running keeps running.
 */
static Lisp l_projectLspForget(Lisp args, LispError *e, LispContext ctx) {
  const Project *p;
  int forgotten;
  (void)args;
  (void)e;
  (void)ctx;
  if (E.current_project < 0) {
    editorSetStatusMessage("Not in a project");
    return lisp_null();
  }
  p = &E.projects[E.current_project];
  forgotten = forgetLspChoices(p->root);
  if (forgotten)
    editorSetStatusMessage("%s: language server choices forgotten, asked again "
                           "next session",
                           baseName(p->root));
  else
    editorSetStatusMessage("%s: no language server choice remembered",
                           baseName(p->root));
  return lisp_null();
}

/** @brief project-switch's on-select: the chosen root becomes current. */
static Lisp projectSwitchSelected(Lisp args, LispError *e, LispContext ctx) {
  const char *root;
  int index;
  (void)e;
  (void)ctx;
  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING)
    return lisp_null();
  root = lisp_string(lisp_car(args));
  index = findProject(root);
  if (index == -1 || E.projects[index].removed)
    return lisp_null();
  activate(index);
  fileFinderOpenAt(E.projects[index].root);
  return lisp_null();
}

/**
 * @brief (project-switch) — lists the remembered projects, most recent
 * first; choosing one makes it current and opens find-file in it.
 */
static Lisp l_projectSwitch(Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  openProjectPicker("Projects", projectSwitchSelected, ctx);
  return lisp_null();
}

void projectRegisterBuiltins(void) {
  registerBuiltin("project-root", l_projectRoot);
  registerBuiltin("project-name", l_projectName);
  registerBuiltin("project-list", l_projectList);
  registerBuiltin("project-add-marker", l_projectAddMarker);
  registerBuiltin("project-trust", l_projectTrust);
  registerBuiltin("project-open-config", l_projectOpenConfig);
  registerBuiltin("project-switch", l_projectSwitch);
  registerBuiltin("project-add", l_projectAdd);
  registerBuiltin("project-remove", l_projectRemove);
  registerBuiltin("project-prune", l_projectPrune);
  registerBuiltin("project-untrust", l_projectUntrust);
  registerBuiltin("project-lsp-forget", l_projectLspForget);
}
