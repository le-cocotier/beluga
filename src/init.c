#include "../include/init.h"
#include "../include/builtins.h"
#include "../include/color.h"
#include "../include/completion.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/file_finder.h"
#include "../include/galloc.h"
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
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
#ifdef _WIN32
  // dired's file operations: Windows has no rm/mv/cp/mkdir to run.
  registerBuiltin("delete-path", l_deletePath);
  registerBuiltin("move-paths", l_movePaths);
  registerBuiltin("copy-paths", l_copyPaths);
  registerBuiltin("make-directories", l_makeDirectories);
#endif
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

  E.ctx = lisp_init();
  E.ctx.p->err_port = fopen("lisp_log.err", "w");
  lisp_lib_load(E.ctx);
  // Init builtins lisp functions
  initBuiltins();

  // Read config file
  E.fd_init_file = fopen(E.init_file_path, "r");
  if (!E.fd_init_file) {
    die("init failed");
  }
  E.ctx_data = lisp_read_file(E.fd_init_file, &E.ctx_error, E.ctx);
  if (E.ctx_error != LISP_ERROR_NONE) {
    die("init failed");
  }
  lisp_eval(E.ctx_data, &E.ctx_error, E.ctx);
  fclose(E.fd_init_file);
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
  E.constantes.LSP = lisp_bool(
      lisp_eval(lisp_read("LSP", &E.ctx_error, E.ctx), &E.ctx_error, E.ctx));
  E.constantes.LINE_NUMBERS = readBoolConst("LINE-NUMBERS", 1);
  E.constantes.WHICH_KEY = readBoolConst("WHICH-KEY", 1);
  appDebug("LSP ON : %d", E.constantes.LSP);
  E.constantes.TAB_LENGTH =
      (int)lisp_eval(lisp_read("TAB-LENGTH", &E.ctx_error, E.ctx), &E.ctx_error,
                     E.ctx)
          .val.int_val;
  E.constantes.QUIT_TIMES =
      (int)lisp_eval(lisp_read("QUIT-TIMES", &E.ctx_error, E.ctx), &E.ctx_error,
                     E.ctx)
          .val.int_val;
}

/**
 * @brief Init all editor variable
 * @note Dynamic allocation for :\n
 *   - E.status_msg \n
 *   - E.prefix
 *   - E.panes.layout (via SplitScreenInit)
 */
void initEditor() {
  const char *home = getenv("HOME");
  EditorPane *active = NULL;

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

  if (asprintf(&E.init_file_path, "%s/.beluga/config/init.lisp", home) == -1) {
    die("asprintf failed");
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
  E.theme.BACKGROUND_COLOR = ANSI_BG_RGB(40, 44, 52);
  E.theme.LINE_CURSOR_BACKGROUND = ANSI_BG_RGB(60, 64, 72);
  E.theme.COLOR_KEYWORD = ANSI_FG_RGB(198, 120, 221);
  E.theme.COLOR_TYPE = ANSI_FG_RGB(97, 175, 239);
  E.theme.COLOR_STRING = ANSI_FG_RGB(152, 195, 121);
  E.theme.COLOR_COMMENT = ANSI_FG_RGB(92, 99, 112);
  E.theme.COLOR_NUMBER = ANSI_FG_RGB(209, 154, 102);
  E.theme.COLOR_DEFAULT = ANSI_FG_RGB(171, 178, 191);

  initConfig();
  initStartupConstants();
  E.lsp_client = (LspClient *)galloc(sizeof(LspClient));
  E.lsp_client->state = LSP_NOT_STARTED;
  // Both left as garbage otherwise until lspStart() actually runs, but
  // editorRunLoop's select() reads read_from_server_fd unconditionally
  // as soon as LSP is enabled — well before the user is even asked
  // whether to start a server.
  E.lsp_client->read_from_server_fd = -1;
  E.lsp_client->write_to_server_fd = -1;
  // Also left as garbage otherwise — lspSyncDebounceTick() (completion.c)
  // reads sync_debounce_pending/deadline before any lspScheduleCompletion-
  // style call would ever overwrite them, since it self-arms rather than
  // waiting to be told to. An uninitialized deadline that happens to land
  // far in the future would silently stall diagnostics resyncing forever.
  E.lsp_client->sync_debounce_pending = 0;
  E.lsp_client->sync_debounce_deadline_ms = 0;
  E.lsp_client->sync_debounce_buffer_id = -1;

  E.quit_times_buffer = E.constantes.QUIT_TIMES;
}

void deInitEditor() {
  int i;
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

  // free lsp_client
  if (E.lsp_client) {
    lspCleanup(E.lsp_client);
  }

  // free config
  fclose(E.ctx.p->err_port);

  lisp_shutdown(E.ctx);
}
