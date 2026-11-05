/**
 * @file lsp_install.c
 * @brief Offers to install a language server that isn't on PATH, and runs
 * the install in a terminal pane.
 * @details The flow, for a file whose (lsp-server ...) program is missing:
 * bufferMaybeStartLsp() -> lspInstallOffer() asks "Install it with ...?";
 * a yes queues the install (the file is still being opened, so no pane is
 * touched from there). The next tick opens *lsp-install* running the
 * command; the tick after it ends checks PATH again and, if the server is
 * there now, asks the usual "Start the ... language server?" for that file.
 */

#include "../include/lsp_install.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/editor_op.h"
#include "../include/input.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/pty_session.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

#define INSTALL_BUFFER "*lsp-install*"

typedef struct {
  char *command; // the server's program, as (lsp-server ...) names it
  char *os;      // "linux", "darwin", "windows", "unix" or "any"
  char *shell;   // what installs it, run by the platform's shell
} InstallRecipe;

static InstallRecipe *s_recipes;
static int s_recipe_count;

// A message for after the file has opened: lspInstallOffer() runs while it
// opens, and the "Opened: ..." that follows would replace it at once.
static char s_notice[512];

enum { JOB_IDLE, JOB_QUEUED, JOB_RUNNING };

// One install at a time: it has the *lsp-install* pane to itself.
static struct {
  int state;
  char *command;
  char *shell;
  int file_buffer_id;     // the file that asked for it, to start the server for
  int terminal_buffer_id; // *lsp-install*, while running
} s_job = {JOB_IDLE, NULL, NULL, -1, -1};

/**
 * @brief (lsp-server-install "command" "os" "shell command" ["os" "shell command"]...)
 * @details e.g. (lsp-server-install "pyright-langserver" "any" "npm install -g pyright").
 * Several commands for one os are tried in order: the first whose program
 * is installed is the one offered (apt-get, then dnf, then pacman...).
 * Calls add up, and a later call's commands are tried first: personal-config,
 * loaded after the packages, can put its own ahead of theirs.
 */
Lisp lspRegisterInstall(Lisp args, LispError *e, LispContext ctx) {
  const char *command;
  const char *os;
  const char *shell;
  InstallRecipe *grown;
  int at = 0; // where this call's next command goes: ahead of earlier calls'
  (void)ctx;

  if (!lisp_is_pair(args) || lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = lisp_is_pair(args) ? LISP_ERROR_ARG_TYPE : LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  command = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  if (!lisp_is_pair(args)) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  while (lisp_is_pair(args)) {
    if (!lisp_is_pair(lisp_cdr(args))) {
      *e = LISP_ERROR_TOO_FEW_ARGS; // an os without its command
      return lisp_null();
    }
    if (lisp_type(lisp_car(args)) != LISP_STRING ||
        lisp_type(lisp_car(lisp_cdr(args))) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    os = lisp_string(lisp_car(args));
    shell = lisp_string(lisp_car(lisp_cdr(args)));
    args = lisp_cdr(lisp_cdr(args));

    grown = grealloc(s_recipes, (size_t)(s_recipe_count + 1) * sizeof(*s_recipes));
    if (!grown)
      return lisp_null();
    s_recipes = grown;
    memmove(s_recipes + at + 1, s_recipes + at,
            (size_t)(s_recipe_count - at) * sizeof(*s_recipes));
    s_recipes[at].command = gstrdup(command);
    s_recipes[at].os = gstrdup(os);
    s_recipes[at].shell = gstrdup(shell);
    s_recipe_count++;
    at++;
  }
  return lisp_null();
}

static int osMatches(const char *entry, const char *os) {
  if (strcmp(entry, "any") == 0 || strcmp(entry, os) == 0)
    return 1;
  return strcmp(entry, "unix") == 0 && strcmp(os, "windows") != 0;
}

/**
 * @brief The program a shell command runs: its first word, or the second
 * after a leading "sudo". Into `out`.
 */
static void programOf(const char *shell, char *out, size_t size) {
  const char *p = shell;
  size_t n;
  for (;;) {
    while (*p == ' ' || *p == '\t')
      p++;
    n = strcspn(p, " \t;&|");
    if (n == 4 && strncmp(p, "sudo", 4) == 0) {
      p += 4;
      continue;
    }
    break;
  }
  if (n >= size)
    n = size - 1;
  memcpy(out, p, n);
  out[n] = '\0';
}

const char *lspInstallCommandFor(const char *command, const char *os, const char **needs) {
  static char first_needed[128];
  char program[128];
  char *found;
  int i;

  if (needs)
    *needs = NULL;
  first_needed[0] = '\0';
  for (i = 0; i < s_recipe_count; i++) {
    if (strcmp(s_recipes[i].command, command) != 0 || !osMatches(s_recipes[i].os, os))
      continue;
    programOf(s_recipes[i].shell, program, sizeof(program));
    found = program[0] ? os_find_program(program) : NULL;
    if (found) {
      gfree(found);
      return s_recipes[i].shell;
    }
    if (!first_needed[0])
      snprintf(first_needed, sizeof(first_needed), "%s", program);
  }
  if (needs && first_needed[0])
    *needs = first_needed;
  return NULL;
}

/**
 * @brief `s` with each % doubled, for a format string (galloc'd).
 */
static char *escapePercent(const char *s) {
  size_t n = strlen(s);
  size_t i;
  size_t j = 0;
  char *out = galloc(2 * n + 1);
  if (!out)
    return NULL;
  for (i = 0; i < n; i++) {
    out[j++] = s[i];
    if (s[i] == '%')
      out[j++] = '%';
  }
  out[j] = '\0';
  return out;
}

static void jobReset(void) {
  gfree(s_job.command);
  gfree(s_job.shell);
  s_job.command = NULL;
  s_job.shell = NULL;
  s_job.state = JOB_IDLE;
  s_job.file_buffer_id = -1;
  s_job.terminal_buffer_id = -1;
}

int lspInstallOffer(struct buffer_t *buf, const struct lspServerConfig_t *cfg) {
  const char *command = cfg->argv[0];
  const char *shell;
  const char *needs;
  char *found;
  char *shown_command;
  char *shown_shell;
  char question[1024];
  Utf8Char *answer;
  int yes;

  found = os_find_program(command);
  if (found) {
    gfree(found);
    return 0;
  }

  if (s_job.state != JOB_IDLE) {
    snprintf(s_notice, sizeof(s_notice),
             "%s isn't installed; another language server is being installed", command);
    return 1;
  }
  shell = lspInstallCommandFor(command, os_name(), &needs);
  if (!shell) {
    if (needs)
      snprintf(s_notice, sizeof(s_notice), "%s isn't installed, and installing it needs %s",
               command, needs);
    else
      snprintf(s_notice, sizeof(s_notice), "%s isn't installed (not found on PATH)", command);
    return 1;
  }

  // The question is a format string: no % from the names.
  shown_command = escapePercent(command);
  shown_shell = escapePercent(shell);
  snprintf(question, sizeof(question), "%s isn't installed. Install it with `%s`? (y/n): %%u",
           shown_command ? shown_command : "The server", shown_shell ? shown_shell : "?");
  gfree(shown_command);
  gfree(shown_shell);
  answer = editorPrompt(question, NULL, 0);
  yes = answer && answer->length == 1 &&
        (answer->character[0] == 'y' || answer->character[0] == 'Y');
  gfree(answer);
  if (!yes) {
    snprintf(s_notice, sizeof(s_notice), "Not installing %s", command);
    return 1;
  }

  s_job.command = gstrdup(command);
  s_job.shell = gstrdup(shell);
  if (!s_job.command || !s_job.shell) {
    jobReset();
    return 1;
  }
  s_job.file_buffer_id = buf->buffer_id;
  s_job.state = JOB_QUEUED;
  return 1;
}

/**
 * @brief The command for the shell: it first echoes what it's about to
 * run, so the pane shows it above the installer's own output. galloc'd.
 */
static char *scriptFor(const char *shell) {
  char *line = utilFormat("$ %s", shell);
  char *quoted = line ? os_shell_quote(line) : NULL;
  char *script = quoted ? utilFormat("echo %s\n%s", quoted, shell) : NULL;
  gfree(line);
  gfree(quoted);
  return script;
}

/**
 * @brief Shows buffer `id` in a new pane below the active one, focused,
 * with the terminal's keymap.
 */
static void showInstallPane(int id) {
  ScreenLayout *layout;
  struct prefix_t prefix;
  int pane;
  int i;
  if (splitScreenSplitActive(SPLIT_HORIZONTAL) != 0) {
    splitScreenGetActivePane()->buffer_id = id; // no room: replace it
    return;
  }
  layout = splitScreenGetLayout();
  pane = layout->active_pane;
  layout->panes[pane].buffer_id = id;
  layout->panes[pane].shown_buffer_id = -1;
  for (i = 0; i < layout->num_panes; i++)
    layout->panes[i].is_active = 0;
  layout->panes[pane].is_active = 1;
  prefix = find_prefix("term");
  if (prefix.prefix_id != -1)
    E.prefix_state = prefix.prefix_id;
}

static void startJob(void) {
  char *script = scriptFor(s_job.shell);
  char **argv = script ? os_shell_argv(script) : NULL;
  Utf8Char *name = stringToUtf8((unsigned char *)INSTALL_BUFFER);
  struct buffer_t *old;
  int id = -1;

  gfree(script);
  old = name ? bufferFindById(bufferFindByFilename(name)) : NULL;
  if (old) {
    ptySessionClose(old); // a live one would make bufferClose() ask
    bufferClose(old->buffer_id);
  }
  if (argv && name) {
    ptySessionSetNextProgram((const char *const *)argv, NULL, NULL);
    id = bufferCreate(name, READ_AND_WRITE, TERMINAL_BUFF);
    ptySessionSetNextProgram(NULL, NULL, NULL);
  }
  os_free_argv(argv);
  gfree(name);

  if (id < 0 || !bufferFindById(id)) {
    editorSetStatusMessage("Couldn't start the install of %s", s_job.command);
    jobReset();
    return;
  }
  showInstallPane(id);
  s_job.terminal_buffer_id = id;
  s_job.state = JOB_RUNNING;
  editorSetStatusMessage("Installing %s in " INSTALL_BUFFER
                         ": answer it there if it asks, ESC for the editor's keys",
                         s_job.command);
}

/**
 * @brief Focuses the pane showing buffer `id`, if one does.
 */
static void focusBuffer(int id) {
  ScreenLayout *layout = splitScreenGetLayout();
  int i;
  for (i = 0; i < layout->num_panes; i++)
    if (layout->panes[i].buffer_id == id) {
      splitScreenFocusPane(i);
      return;
    }
}

static void finishJob(void) {
  struct lspServerConfig_t cfg;
  struct buffer_t *file;
  LspClient *lsp;
  char *found = os_find_program(s_job.command);
  char *shown = escapePercent(s_job.command);
  int file_id = s_job.file_buffer_id;

  // Asked again at the next file either way: started now if it's there,
  // offered again if it still isn't.
  memset(&cfg, 0, sizeof(cfg));
  cfg.argv[0] = s_job.command;
  lsp = lspClientForConfig(&cfg);
  if (lsp && lsp->state == LSP_SHUTDOWN && !lsp->proc)
    lsp->state = LSP_NOT_STARTED;
  jobReset();

  if (!found) {
    editorSetStatusMessage("%s still isn't on PATH: see " INSTALL_BUFFER " for what happened",
                           shown ? shown : "The server");
    gfree(shown);
    return;
  }
  gfree(found);
  editorSetStatusMessage("%s is installed", shown ? shown : "The server");
  gfree(shown);

  file = bufferFindById(file_id);
  if (file && file->type == FILE_BUFF) {
    focusBuffer(file_id);
    bufferMaybeStartLsp(file);
  }
}

void lspInstallTick(void) {
  struct buffer_t *terminal;
  if (s_notice[0]) {
    editorSetStatusMessage("%s", s_notice);
    s_notice[0] = '\0';
  }
  if (s_job.state == JOB_QUEUED) {
    startJob();
    return;
  }
  if (s_job.state != JOB_RUNNING)
    return;
  terminal = bufferFindById(s_job.terminal_buffer_id);
  // The installer has exited (the pane keeps its output), or the pane was
  // killed.
  if (!terminal || terminal->type != TERMINAL_BUFF || !terminal->pty)
    finishJob();
}

void lspInstallFree(void) {
  int i;
  for (i = 0; i < s_recipe_count; i++) {
    gfree(s_recipes[i].command);
    gfree(s_recipes[i].os);
    gfree(s_recipes[i].shell);
  }
  gfree(s_recipes);
  s_recipes = NULL;
  s_recipe_count = 0;
  jobReset();
}
