#include "../include/init.h"
#include "../include/breakpoints.h"
#include "../include/builtins.h"
#include "../include/dap.h"
#include "../include/color.h"
#include "../include/cJSON.h"
#include "../include/completion.h"
#include "../include/config_load.h"
#include "../include/data.h"
#include "../include/describe.h"
#include "../include/settings.h"
#include "../include/define.h"
#include "../include/file_finder.h"
#include "../include/galloc.h"
#include "../include/macro.h"
#include "../include/packages.h"
#include "../include/project.h"
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

// From syntax_highlighter.h, which can't be included here: its TokenType
// clashes with the one lisp.h's implementation (LISP_IMPLEMENTATION, above)
// defines.
void syntaxFreeCommentWords(void);

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
  registerBuiltin("package-loaded?", packageLoaded);
  registerBuiltin("package-state", l_packageState);
  registerBuiltin("package-version", l_packageVersion);
  registerBuiltin("package-directory", l_packageDirectory);
  registerBuiltin("beluga-directory", l_belugaDirectory);
  registerBuiltin("beluga-version", l_belugaVersion);
  registerBuiltin("version-compare", l_versionCompare);
  registerBuiltin("read-data-file", l_readDataFile);
  registerBuiltin("write-data-file", l_writeDataFile);
  registerBuiltin("editor-redisplay", l_editorRedisplay);
  registerBuiltin("editor-initialized?", l_editorInitialized);
  registerBuiltin("editor-batch?", l_editorBatch);
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
  registerBuiltin("lsp-document-symbols", lspDocumentSymbols);
  registerBuiltin("lsp-workspace-symbols", lspWorkspaceSymbols);
  registerBuiltin("lsp-expand-macro", lspExpandMacro);
  registerBuiltin("lsp-struct-layout", lspStructLayout);
  registerBuiltin("lsp-incoming-calls", lspIncomingCalls);
  registerBuiltin("lsp-goto-location", lspGotoLocationBuiltin);
  registerBuiltin("lsp-signature-help", lspSignatureHelp);
  registerBuiltin("lsp-code-action", lspCodeAction);
  registerBuiltin("lsp-apply-code-action", lspApplyCodeAction_L);
  registerBuiltin("lsp-rename", lspRename);
  registerBuiltin("lsp-format-buffer", lspFormatBuffer);
  registerBuiltin("editor-open-terminal", editorOpenTerminal);
  registerBuiltin("editor-undo", editorUndo);
  registerBuiltin("macro-start", macroStart);
  registerBuiltin("macro-end", macroEnd);
  registerBuiltin("macro-run", macroRunBuiltin);
  registerBuiltin("editor-redraw", editorRedraw);
  registerBuiltin("describe-key", describeKey);
  registerBuiltin("key-bindings", keyBindings);
  registerBuiltin("describe-function", describeFunction);
  registerBuiltin("describe-variable", describeVariable);
  registerBuiltin("settings", settingsList);
  registerBuiltin("editor-redo", editorRedo);
  registerBuiltin("editor-set-mark", editorSetMark);
  registerBuiltin("editor-kill-region", editorKillRegion);
  registerBuiltin("editor-cut-region", editorCutRegion);
  registerBuiltin("editor-copy-region", editorCopyRegion);

  registerBuiltin("add-hook", addHook);
  registerBuiltin("line-text", lineTextAt);
  registerBuiltin("buffer-insert-string", l_bufferInsertString);
  registerBuiltin("buffer-delete-range", l_bufferDeleteRange);
  registerBuiltin("buffer-replace-range", l_bufferReplaceRange);
  registerBuiltin("buffer-text", l_bufferText);
  registerBuiltin("set-cursor", l_setCursor);
  registerBuiltin("region-bounds", l_regionBounds);
  registerBuiltin("buffer-local-set", l_bufferLocalSet);
  registerBuiltin("buffer-local", l_bufferLocal);
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
  // project-root, project-list, project-trust, ... (project.c)
  projectRegisterBuiltins();
  registerBuiltin("syntax-define-language", l_syntaxDefineLanguage);
  registerBuiltin("syntax-add-keyword", l_syntaxAddKeyword);
  registerBuiltin("syntax-add-type", l_syntaxAddType);
  registerBuiltin("syntax-add-comment-word", l_syntaxAddCommentWord);
  registerBuiltin("syntax-add-line-prefix", l_syntaxAddLinePrefix);
  registerBuiltin("buffer-set-syntax", l_bufferSetSyntax);
  registerBuiltin("write-file", l_writeFile);
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
  registerBuiltin("debug-toggle-breakpoint", l_debugToggleBreakpoint);
  registerBuiltin("debug-breakpoint-condition", l_debugBreakpointCondition);
  registerBuiltin("debug-breakpoint-hit-count", l_debugBreakpointHitCount);
  registerBuiltin("debug-logpoint", l_debugLogpoint);
  registerBuiltin("debug-exception-breakpoints", l_debugExceptionBreakpoints);
  registerBuiltin("debug-clear-breakpoints", l_debugClearBreakpoints);
  registerBuiltin("debug-breakpoints", l_debugBreakpoints);
  registerBuiltin("dap-adapter", l_dapAdapter);
  registerBuiltin("debug-start", l_debugStart);
  registerBuiltin("debug-continue", l_debugContinue);
  registerBuiltin("debug-stop", l_debugStop);
  registerBuiltin("debug-next", l_debugNext);
  registerBuiltin("debug-step-in", l_debugStepIn);
  registerBuiltin("debug-step-out", l_debugStepOut);
  registerBuiltin("debug-pause", l_debugPause);
  registerBuiltin("debug-panel", l_debugPanel);
  registerBuiltin("debug-panel-select", l_debugPanelSelect);
  registerBuiltin("debug-output", l_debugOutput);
  registerBuiltin("debug-evaluate", l_debugEvaluate);
  registerBuiltin("debug-command", l_debugCommand);
  registerBuiltin("debug-state", l_debugState);
  registerBuiltin("debug-frames", l_debugFrames);
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
  char *home;

  E.ctx = lisp_init();
  // In ~/.beluga, not the directory beluga was started from (where it
  // used to leave a lisp_log.err behind in every project). w+:
  // config_load.c reads back what the interpreter wrote here to put the
  // reason for a failing form in its error message.
  home = os_home_dir();
  lisp_log_path = home ? utilFormat("%s/.beluga/lisp_log.err", home) : NULL;
  gfree(home);
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
 * @brief Reads the settings (LSP, LINE-NUMBERS, WHICH-KEY, TAB-LENGTH, ...)
 * out of the Lisp environment initConfig() just finished loading, into
 * E.constantes — see the table in settings.c.
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
  settingsLoad();
  appDebug("LSP ON : %d", E.constantes.LSP);
  E.global_constantes = E.constantes;
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
 * @brief A galloc'd copy of a fallback theme color, so that every color in
 * E.theme can be freed, whether a theme package replaced it or not.
 */
static char *themeColor(const char *ansi) {
  char *copy = galloc(strlen(ansi) + 1);
  if (copy)
    strcpy(copy, ansi);
  return copy;
}

/**
 * @brief Init all editor variable
 * @note Dynamic allocation for :\n
 *   - E.status_msg \n
 *   - E.prefix
 *   - E.panes.layout (via SplitScreenInit)
 */
/**
 * @brief Breakpoints outlive the editor: ~/.beluga/breakpoints holds every
 * project's, and each debug session gets those of its own project.
 */
static void initBreakpointStore(void) {
  char path[OS_PATH_MAX];
  const char *dir = utilBelugaDir();
  if (!dir)
    return;
  snprintf(path, sizeof(path), "%s/breakpoints", dir);
  breakpointsSetStore(path);
}

void initEditor() {
  char *home;
  EditorPane *active = NULL;

  // Before anything can build a cJSON object (the LSP client, later on).
  initJsonHooks();

  // No terminal to ask in batch mode (and asking writes to stdout).
  if (E.batch || getWindowSize(&E.screenrows, &E.screencols) == -1) {
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

  home = os_home_dir();
  if (!home) {
    die("HOME environment variable not set");
  }
  gfree(home);

  E.init_file_path = utilFormat("%s/config/init.lisp", utilBelugaDir());
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
  E.prompt_matches = NULL;
  E.prompt_match_count = 0;
  E.prompt_match_selected = -1;
  E.exit_requested = 0;

  // Fallback theme (the old hardcoded "dark" palette), live before
  // initConfig() runs — a theme package's editor-set-theme-color calls
  // overwrite these during config load, but there's always something
  // valid in E.theme even if no theme package is installed/loaded.
  E.theme.BACKGROUND_COLOR = themeColor(ANSI_BG_RGB(40, 44, 52));
  E.theme.LINE_CURSOR_BACKGROUND = themeColor(ANSI_BG_RGB(60, 64, 72));
  E.theme.COLOR_KEYWORD = themeColor(ANSI_FG_RGB(198, 120, 221));
  E.theme.COLOR_TYPE = themeColor(ANSI_FG_RGB(97, 175, 239));
  E.theme.COLOR_STRING = themeColor(ANSI_FG_RGB(152, 195, 121));
  E.theme.COLOR_COMMENT = themeColor(ANSI_FG_RGB(92, 99, 112));
  E.theme.COLOR_NUMBER = themeColor(ANSI_FG_RGB(209, 154, 102));
  E.theme.COLOR_DEFAULT = themeColor(ANSI_FG_RGB(171, 178, 191));
  E.theme.COLOR_ADDED = themeColor(ANSI_FG_RGB(152, 195, 121));
  E.theme.COLOR_REMOVED = themeColor(ANSI_FG_RGB(224, 108, 117));
  E.theme.DEBUG_LINE_BACKGROUND = themeColor(ANSI_BG_RGB(77, 68, 38));

  E.projects = NULL;
  E.number_of_projects = 0;
  E.current_project = -1;

  E.initialized = 0;
  initConfig();
  E.initialized = 1;
  initStartupConstants();
  // After init.lisp (which may add project markers) and its settings,
  // which projects without a config of their own keep.
  projectInit();
  initBreakpointStore();
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

  packagesFree();

  // free prefix
  gfree(E.prefix);

  // free the project-search result list
  projectSearchFree();

  // The debug session ends before the Lisp interpreter does.
  dapFree();
  debugBuiltinsFree();
  breakpointsFree();

  // write back ~/.beluga/projects, then free the list
  projectFree();

  // free the fuzzy file finder's path snapshot
  fileFinderClose();

  // free syntax-highlighter language registrations (each has its own
  // separately-allocated word list)
  for (i = 0; i < E.number_of_languages; i++)
    gfree(E.languages[i].words);
  gfree(E.languages);
  syntaxFreeCommentWords();

  // free the keyboard macro
  macroFree();

  // Shut every LSP server down before freeing any client: shutting one
  // down looks up which client each buffer belongs to, which walks all of
  // them.
  for (i = 0; i < E.number_of_lsp_clients; i++)
    lspShutdown(E.lsp_clients[i]);
  for (i = 0; i < E.number_of_lsp_clients; i++)
    lspCleanup(E.lsp_clients[i]);
  E.number_of_lsp_clients = 0;

  temp = &E.theme.BACKGROUND_COLOR;
  for (i = 0; i < THEME_COLOR_COUNT; ++i) {
    gfree(*(temp + i));
  }

  configErrorsFree();

  // free config
  fclose(E.ctx.p->err_port);

  lisp_shutdown(E.ctx);
}
