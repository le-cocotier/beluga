#include "../include/init.h"
#include "../include/builtins.h"
#include "../include/cJSON.h"
#include "../include/color.h"
#include "../include/completion.h"
#include "../include/config_load.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/file_finder.h"
#include "../include/galloc.h"
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Vendored interpreter: its implementation mixes declarations and code,
// which -Wdeclaration-after-statement (meson.build) would flag ~100 times.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#define LISP_IMPLEMENTATION
#include "../include/lisp.h"
#include "../include/lisp_lib.h"
#pragma GCC diagnostic pop

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/

void registerBuiltin(const char *key_sequence, const LispCFunc f) {
  lisp_env_define(E.ctx.p->env, lisp_make_symbol(key_sequence, E.ctx),
                  lisp_make_func(f), E.ctx);
}

void initBuiltins(void) {
  // Registering C functions as lisp macro
  registerBuiltin("move-cursor", moveCursor);
  registerBuiltin("map-key", mapKey);
  registerBuiltin("editor-quit", editorQuit);
  registerBuiltin("editor-save", l_editorSave);
  registerBuiltin("editor-insert-new-line", l_editorInsertNewLine);
  registerBuiltin("move-cursor-beg-line", moveCursorBeginLine);
  registerBuiltin("move-cursor-end-line", moveCursorEndLine);
  registerBuiltin("editor-delete-previous-char", deletePreviousChar);
  registerBuiltin("move-cursor-page-up", editorMoveCursorPageUp);
  registerBuiltin("move-cursor-page-down", editorMoveCursorPageDown);
  registerBuiltin("editor-open-file", editorOpenFile);
  registerBuiltin("editor-open-config", editorOpenConfig);
  registerBuiltin("editor-set-theme-color", editorSetThemeColor);
  registerBuiltin("editor-insert-char", editorPrintC);
  registerBuiltin("add-package", addPackage);
  registerBuiltin("buffer-find", bufferFind_L);
  registerBuiltin("buffer-find-reverse", bufferFindReverse_L);
  registerBuiltin("editor-read-char", editorReadChar_L);
  registerBuiltin("add-prefix", editorPrefix);
  registerBuiltin("editor-set-prefix", editorSetPrefix);
  registerBuiltin("editor-insert-tab", l_editorInserTab);
  registerBuiltin("editor-switch-next-buffer", editorSwitchNextBuffer);
  registerBuiltin("editor-switch-prev-buffer", editorSwitchPreviousBuffer);
  registerBuiltin("editor-split-screen-vertical", l_editorSplitScreenVertical);
  registerBuiltin("editor-split-screen-horizontal",
                  l_editorSplitScreenHorizontal);
  registerBuiltin("editor-switch-next-pane", editorSwitchNextPane);
  registerBuiltin("editor-unify-panes", editorUnifiedPanes);
  registerBuiltin("editor-paste", editorPaste);
  registerBuiltin("editor-cut-end-line", editorCutEndLine);
  registerBuiltin("editor-move-cursor-beg-buffer", editorMoveBegBuffer);
  registerBuiltin("editor-move-cursor-end-buffer", editorMoveEndBuffer);
  registerBuiltin("editor-auto-complete", editorAutoComplete);
  registerBuiltin("editor-get-pane-width", editorGetPaneWitdh);
  registerBuiltin("editor-get-pane-height", editorGetPaneHeight);
  // registerBuiltin("lsp-complete",    lspComplete);
  registerBuiltin("lsp-definition", lspDefinition);
  registerBuiltin("lsp-hover", lspHover);
  registerBuiltin("lsp-references", lspReferences);
  registerBuiltin("lsp-diagnostics", lspDiagnostics);
  registerBuiltin("lsp-signature-help", lspSignatureHelp);
  registerBuiltin("lsp-code-action", lspCodeAction);
  registerBuiltin("lsp-apply-code-action", lspApplyCodeAction_L);
  registerBuiltin("lsp-rename", lspRename);
  registerBuiltin("lsp-format-buffer", lspFormatBuffer);
  registerBuiltin("editor-open-terminal", editorOpenTerminal);
  registerBuiltin("editor-undo", editorUndo);
  registerBuiltin("editor-redo", editorRedo);
  registerBuiltin("editor-set-mark", editorSetMark);
  registerBuiltin("editor-kill-region", editorKillRegion);
  registerBuiltin("editor-cut-region", editorCutRegion);
  registerBuiltin("editor-copy-region", editorCopyRegion);

  registerBuiltin("add-hook", addHook);
  registerBuiltin("line-text", lineTextAt);
  registerBuiltin("cursor-row", cursorRowLisp);
  registerBuiltin("cursor-col", cursorColLisp);
  registerBuiltin("buffer-line-count", bufferLineCountLisp);
  registerBuiltin("buffer-filename", bufferFilenameLisp);
  registerBuiltin("lsp-server", lspRegisterServer);
  registerBuiltin("execute-command", editorExecuteCommand);
  registerBuiltin("editor-copy-end-line", editorCopyEndLine);
  registerBuiltin("editor-switch-buffer-popup", editorSwitchBufferPopup);
  registerBuiltin("editor-goto-line", editorGotoLine);
  registerBuiltin("editor-list-popup", editorListPopup);
  registerBuiltin("editor-open-file-path", editorOpenFilePath);
  registerBuiltin("list-buffers", l_listBuffers);
  registerBuiltin("editor-switch-to-buffer", editorSwitchToBuffer);
  registerBuiltin("editor-kill-buffer", editorKillBuffer);
  registerBuiltin("editor-pwd", editorPwd);
  registerBuiltin("project-root", l_projectRoot);
  registerBuiltin("editor-prompt", editorPromptLisp);
  registerBuiltin("list-directory", listDirectory);
  registerBuiltin("editor-scratch-open", editorScratchOpen);
  registerBuiltin("editor-scratch-set-lines", editorScratchSetLines);
  registerBuiltin("editor-explorer-sidebar-open", editorExplorerSidebarOpen);
  registerBuiltin("editor-explorer-close", editorExplorerClose);
  registerBuiltin("project-search", l_projectSearch);
  registerBuiltin("project-search-goto", l_projectSearchGoto);
  registerBuiltin("query-replace", l_queryReplace);
  registerBuiltin("project-query-replace", l_projectQueryReplace);
  registerBuiltin("find-file", l_findFile);
  registerBuiltin("syntax-define-language", l_syntaxDefineLanguage);
  registerBuiltin("syntax-add-keyword", l_syntaxAddKeyword);
  registerBuiltin("syntax-add-type", l_syntaxAddType);
  registerBuiltin("app-debug?", l_appDebugP);
  registerBuiltin("editor-recenter", l_editorRecenter);
  registerBuiltin("pane-height", l_paneHeight);
  registerBuiltin("pane-width", l_paneWidth);
  registerBuiltin("expand-path", l_expandPath);
  registerBuiltin("file-attributes", l_fileAttributes);
  registerBuiltin("process-run", l_processRun);
  registerBuiltin("system-type", l_systemType);
  registerBuiltin("shell-quote", l_shellQuote);
  registerBuiltin("shell-run", l_shellRun);
  // File operations (dired's D, R, C, +): native, not rm/mv/cp/mkdir.
  registerBuiltin("delete-path", l_deletePath);
  registerBuiltin("move-paths", l_movePaths);
  registerBuiltin("copy-paths", l_copyPaths);
  registerBuiltin("make-directories", l_makeDirectories);
  registerBuiltin("process-start", l_processStart);
  registerBuiltin("string-split", l_stringSplit);
  registerBuiltin("current-buffer-id", l_currentBufferId);
  registerBuiltin("editor-message", l_editorMessage);
  registerBuiltin("editor-scratch-set-prefix", l_editorScratchSetPrefix);
#ifdef APP_DEBUG
  registerBuiltin("memory-inspector", l_memoryInspectorOpen);
  registerBuiltin("memory-inspector-refresh", l_memoryInspectorRefresh);
  registerBuiltin("memory-inspector-sort", l_memoryInspectorCycleSort);
  registerBuiltin("memory-inspector-export", l_memoryInspectorExport);
#endif
}

/**
 * @brief The bindings needed to fix a broken config from inside beluga:
 * move around, type, save, open a file and quit.
 * @details Applied only after init.lisp reported an error, and only for
 * keys the config didn't bind itself, so a config that half-loaded keeps
 * whatever it did set up. A missing init.lisp ends up with exactly these.
 */
static const char *const EMERGENCY_BINDINGS[][3] = {
    {"CTRL-x", "'(editor-set-prefix \"user\")", "no-prefix"},
    {"CTRL-g", "'(editor-set-prefix \"no-prefix\")", "*"},
    {"ARROW-UP", "'(move-cursor \"up\")", "no-prefix"},
    {"ARROW-DOWN", "'(move-cursor \"down\")", "no-prefix"},
    {"ARROW-RIGHT", "'(move-cursor \"right\")", "no-prefix"},
    {"ARROW-LEFT", "'(move-cursor \"left\")", "no-prefix"},
    {"ENTER", "editor-insert-new-line", "no-prefix"},
    {"BACKSPACE", "editor-delete-previous-char", "no-prefix"},
    {"PAGE-UP", "move-cursor-page-up", "no-prefix"},
    {"PAGE-DOWN", "move-cursor-page-down", "no-prefix"},
    {"ALT-x", "execute-command", "no-prefix"},
    {"CTRL-c", "editor-quit", "user"},
    {"CTRL-s", "editor-save", "user"},
    {"f", "editor-open-file", "user"},
    {"c", "editor-open-config", "user"},
    {"b", "editor-switch-buffer-popup", "user"},
};

static int prefixExists(const char *name) {
  int i;
  for (i = 0; i <= E.number_of_prefix; i++)
    if (strcmp(E.prefix[i].prefix_name, name) == 0)
      return 1;
  return 0;
}

static int keyBound(const char *key, const char *prefix_name) {
  const int prefix_id = find_prefix(prefix_name).prefix_id;
  int i;
  for (i = 0; i < E.number_of_keybinds; i++)
    if (E.key_binds[i].prefix_id == prefix_id &&
        strcmp(E.key_binds[i].key_sequence, key) == 0)
      return 1;
  return 0;
}

static void ensureEmergencyBindings(void) {
  char form[256];
  LispError err;
  size_t i;
  if (!prefixExists("user")) {
    err = LISP_ERROR_NONE;
    lisp_eval(lisp_read("(add-prefix \"user\")", &err, E.ctx), &err, E.ctx);
  }
  for (i = 0; i < sizeof(EMERGENCY_BINDINGS) / sizeof(EMERGENCY_BINDINGS[0]);
       i++) {
    if (keyBound(EMERGENCY_BINDINGS[i][0], EMERGENCY_BINDINGS[i][2]))
      continue;
    snprintf(form, sizeof(form), "(map-key \"%s\" %s \"%s\")",
             EMERGENCY_BINDINGS[i][0], EMERGENCY_BINDINGS[i][1],
             EMERGENCY_BINDINGS[i][2]);
    err = LISP_ERROR_NONE;
    lisp_eval(lisp_read(form, &err, E.ctx), &err, E.ctx);
  }
}

/**
 * @brief Sets up the Lisp interpreter, registers all builtins, then reads
 * and evaluates config/init.lisp.
 * @details This is the one call that actually loads the user's config:
 * evaluating init.lisp runs every top-level form in it in sequence, which
 * is what add-package's each package's own init.lisp, runs map-key/add-hook/
 * lsp-server calls, and — via whichever theme package calls
 * editor-set-theme-color — is what sets E.theme's real colors. There is no
 * separate "load the theme" step; it all happens here as a side effect of
 * evaluating Lisp. initStartupConstants(), called right after this returns,
 * only picks a handful of plain (non-color) values back out of the
 * resulting environment.
 */
static void initConfig(void) {
  char *lisp_log_path;

  E.ctx = lisp_init();
  // In ~/.beluga, not the directory beluga was started from (where it
  // used to leave a lisp_log.err behind in every project). w+:
  // config_load.c reads back what the interpreter wrote here to put the
  // reason for a failing form in its error message.
  lisp_log_path = utilFormat("%s/.beluga/lisp_log.err", os_home_dir());
  E.ctx.p->err_port = lisp_log_path ? fopen(lisp_log_path, "w+") : NULL;
  gfree(lisp_log_path);
  if (!E.ctx.p->err_port)
    E.ctx.p->err_port = tmpfile();
  lisp_lib_load(E.ctx);
  // Init builtins lisp functions
  initBuiltins();

  // Form by form: a mistake is reported (see configShowErrors(), called
  // once the first buffer is up) instead of stopping the editor, and the
  // rest of the file still loads.
  configLoadFile(E.init_file_path, "init.lisp");
  if (configErrorCount() > 0)
    ensureEmergencyBindings();
}

/**
 * @brief Read a boolean setting from init.lisp, falling back to `fallback`
 * when the symbol isn't defined at all.
 * @details Uses an env lookup rather than lisp_read+lisp_eval (as the
 * older constants do) precisely so a config predating the setting still
 * loads: evaluating an unbound symbol raises a Lisp error and yields a
 * junk value, which would silently turn the feature on or off at random
 * for every existing ~/.beluga/config/init.lisp out there.
 */
static int readBoolConst(const char *name, int fallback) {
  int present = 0;
  const Lisp val =
      lisp_env_lookup(lisp_env(E.ctx), lisp_make_symbol(name, E.ctx), &present);
  if (!present)
    return fallback;
  return lisp_is_true(val);
}

/**
 * @brief Read an integer setting from init.lisp, falling back to
 * `fallback` when the symbol isn't defined or isn't an integer.
 */
static int readIntConst(const char *name, int fallback) {
  int present = 0;
  const Lisp val =
      lisp_env_lookup(lisp_env(E.ctx), lisp_make_symbol(name, E.ctx), &present);
  if (!present || lisp_type(val) != LISP_INT)
    return fallback;
  return (int)lisp_int(val);
}

/**
 * @brief Reads non-color startup constants (LSP, LINE-NUMBERS, WHICH-KEY,
 * TAB-LENGTH, QUIT-TIMES) out of the Lisp environment initConfig() just
 * finished loading, into E.constantes.
 * @details Despite the historical name this doesn't touch theme *colors*
 * at all — those are set directly onto E.theme by the editor-set-theme-color
 * builtin, called as a side effect of evaluating config/init.lisp inside
 * initConfig() (whichever theme package it add-package's — "theme-dark",
 * "theme-light", or a user's own — calls editor-set-theme-color while its
 * own init.lisp loads). initEditor() seeds E.theme with a hardcoded
 * fallback before initConfig() runs, so there's always a valid theme even
 * if no package sets one. Must run after initConfig(): it looks up symbols
 * (LSP, TAB-LENGTH, ...) that only exist once init.lisp has been evaluated.
 */
static void initStartupConstants(void) {
  // Fallbacks are the bundled config's values, so a config that failed to
  // load (or predates a setting) behaves like the default one.
  E.constantes.LSP = readBoolConst("LSP", 1);
  E.constantes.LINE_NUMBERS = readBoolConst("LINE-NUMBERS", 1);
  E.constantes.WHICH_KEY = readBoolConst("WHICH-KEY", 1);
  E.constantes.LSP_SIGNATURE_HELP = readBoolConst("LSP-SIGNATURE-HELP", 1);
  E.constantes.AUTOSAVE_INTERVAL = readIntConst("AUTOSAVE-INTERVAL", 15);
  appDebug("LSP ON : %d", E.constantes.LSP);
  E.constantes.TAB_LENGTH = readIntConst("TAB-LENGTH", 2);
  E.constantes.QUIT_TIMES = readIntConst("QUIT-TIMES", 1);
}

/**
 * @brief cJSON's allocation hooks: route its nodes and printed strings
 * through galloc, so they show up in the memory inspector and the exit
 * report like everything else. galloc is a macro, hence the wrapper.
 * cJSON_InitHooks() with a non-malloc allocator drops realloc, so cJSON's
 * print buffers grow by allocate + copy + free, all through these two.
 */
static void *cjsonAlloc(size_t size) { return galloc(size); }

static void initJsonHooks(void) {
  cJSON_Hooks hooks;
  hooks.malloc_fn = cjsonAlloc;
  hooks.free_fn = gfree;
  cJSON_InitHooks(&hooks);
}

/**
 * @brief Init all editor variable
 * @note Dynamic allocation for :\n
 *   - E.status_msg \n
 *   - E.prefix
 *   - E.panes.layout (via SplitScreenInit)
 */
void initEditor() {
  const char *home = os_home_dir();
  EditorPane *active = NULL;

  // Before anything can build a cJSON object (the LSP client, later on).
  initJsonHooks();

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    E.screencols = 80;
    E.screenrows = 24;
    //    die("getWindowSize");
  }
  appDebug("%d %d", E.screenrows, E.screencols);
  E.screenrows -= 2;

  // Init graphics variables
  splitScreenInit();
  active = splitScreenGetActivePane();
  E.cursor_x = 0;
  E.cursor_y = 0;

  active->cursor_x = 0;
  active->cursor_y = 0;
  active->origin_x = 0;
  active->origin_y = 0;
  active->width = E.screencols;
  active->height = E.screenrows;

  if (!home) {
    die("HOME environment variable not set");
  }

  E.init_file_path = utilFormat("%s/.beluga/config/init.lisp", home);
  if (!E.init_file_path) {
    die("out of memory");
  }

  E.status_msg_time = 0;
  E.status_msg = gcalloc(PATH_MAX, sizeof(Utf8Char));
  // E.buffers is the head of a circular doubly-linked list, built up node
  // by node in bufferCreate() — NULL/0 means no buffers open yet.
  E.buffers = NULL;
  E.number_of_buffer = 0;
  // Key binds
  E.number_of_keybinds = 0;
  // Hooks — must be zeroed before initConfig() runs, since init.lisp itself
  // may call add-hook while being loaded
  E.hooks = NULL;
  E.number_of_hooks = 0;
  // Same reasoning: init.lisp calls lsp-server while loading
  E.lsp_servers = NULL;
  E.number_of_lsp_servers = 0;
  // ...and syntax-define-language, from every language package
  E.languages = NULL;
  E.number_of_languages = 0;
  E.number_of_prefix = 0;

  // "no-prefix" MUST stay id 0 — every hardcoded `E.prefix_state = 0`
  // reset scattered across the codebase (buffer.c, input.c,
  // project_search.c, split_screen.c, which_key.c) assumes id 0 means
  // "no-prefix" rather than looking it up by name each time. Put any new
  // built-in prefix (like "*" below) after the existing ones instead of
  // renumbering these.
  E.prefix = (struct prefix_t *)galloc(sizeof(struct prefix_t) * 4);
  E.prefix[0].prefix_id = 0;
  E.prefix[0].sticky = 0;
  sprintf(E.prefix[0].prefix_name, "no-prefix");

  // "search" is registered here rather than left to init.lisp because the
  // search results buffer switches to it from C the moment results are
  // shown. A config predating this feature would leave find_prefix()
  // returning -1 there, so the buffer would come up under whatever prefix
  // happened to be active — with ENTER meaning something else entirely.
  // Sticky, so arrow keys can walk the result list without dropping out
  // after the first press.
  E.prefix[1].prefix_id = 1;
  E.prefix[1].sticky = 1;
  sprintf(E.prefix[1].prefix_name, "search");

  // "*" is the global prefix: STAR_PREFIX in input.c matches a keybind
  // registered under it regardless of E.prefix_state, so e.g. CTRL-g can
  // reset to no-prefix from inside any other prefix with one binding
  // instead of one per prefix. Its own id is never assigned to
  // E.prefix_state — nothing ever "enters" the global prefix — so unlike
  // "no-prefix" and "search" above, no other code depends on this
  // particular index; it just has to not collide with them.
  E.number_of_prefix = 2;
  E.prefix[2].prefix_id = 2;
  E.prefix[2].sticky = 0;
  sprintf(E.prefix[2].prefix_name, "*");

  // "memory" is registered here for the same reason "search" is: the
  // memory-inspector buffer (debug builds only, see memory_inspector.c)
  // switches to it from C the instant it's shown. Reserved unconditionally
  // (not #ifdef APP_DEBUG) so the prefix ids after it stay stable
  // regardless of build type. Sticky, so arrow keys/refresh don't drop
  // back out after one keystroke.
  E.number_of_prefix = 3;
  E.prefix[3].prefix_id = 3;
  E.prefix[3].sticky = 1;
  sprintf(E.prefix[3].prefix_name, "memory");

  E.prefix_state = 0;
  E.prompt_input = NULL;
  E.prompt_is_command = 0;
  E.exit_requested = 0;

  // Fallback theme (the old hardcoded "dark" palette), live before
  // initConfig() runs — a theme package's editor-set-theme-color calls
  // overwrite these during config load, but there's always something
  // valid in E.theme even if no theme package is installed/loaded.
  // Copies, not the literals: a theme's editor-set-theme-color frees the
  // color it replaces, and deInitEditor() frees all of them.
  E.theme.BACKGROUND_COLOR = utilStrdup(ANSI_BG_RGB(40, 44, 52));
  E.theme.LINE_CURSOR_BACKGROUND = utilStrdup(ANSI_BG_RGB(60, 64, 72));
  E.theme.COLOR_KEYWORD = utilStrdup(ANSI_FG_RGB(198, 120, 221));
  E.theme.COLOR_TYPE = utilStrdup(ANSI_FG_RGB(97, 175, 239));
  E.theme.COLOR_STRING = utilStrdup(ANSI_FG_RGB(152, 195, 121));
  E.theme.COLOR_COMMENT = utilStrdup(ANSI_FG_RGB(92, 99, 112));
  E.theme.COLOR_NUMBER = utilStrdup(ANSI_FG_RGB(209, 154, 102));
  E.theme.COLOR_DEFAULT = utilStrdup(ANSI_FG_RGB(171, 178, 191));

  initConfig();
  initStartupConstants();
  // Clients are created per server, the first time a matching file is
  // opened (bufferMaybeStartLsp()).
  E.number_of_lsp_clients = 0;

  E.quit_times_buffer = E.constantes.QUIT_TIMES;
}

void deInitEditor() {
  int i;
  char **temp;
  // free EditorPane
  gfree(E.layout.panes);

  // free status_msg
  gfree(E.status_msg);

  // free init_file_path
  gfree(E.init_file_path);

  // free prefix
  gfree(E.prefix);

  // free the project-search result list
  projectSearchFree();

  // free the fuzzy file finder's path snapshot
  fileFinderClose();

  // free syntax-highlighter language registrations (each has its own
  // separately-allocated word list)
  for (i = 0; i < E.number_of_languages; i++)
    gfree(E.languages[i].words);
  gfree(E.languages);

  // Shut every LSP server down before freeing any client: shutting one
  // down looks up which client each buffer belongs to, which walks all of
  // them.
  for (i = 0; i < E.number_of_lsp_clients; i++)
    lspShutdown(E.lsp_clients[i]);
  for (i = 0; i < E.number_of_lsp_clients; i++)
    lspCleanup(E.lsp_clients[i]);
  E.number_of_lsp_clients = 0;

  temp = &E.theme.BACKGROUND_COLOR;
  for (i = 0; i < 8; ++i) {
    gfree(*(temp + i));
  }

  configErrorsFree();

  // free config
  fclose(E.ctx.p->err_port);

  lisp_shutdown(E.ctx);
}
