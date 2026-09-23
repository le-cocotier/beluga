#ifndef BUILTINS_H_
#define BUILTINS_H_

#include "lisp.h"

struct prefix_t find_prefix(const char prefix_name[64]);

Lisp l_projectSearch(Lisp args, LispError *e, LispContext ctx);
Lisp l_projectSearchGoto(Lisp args, LispError *e, LispContext ctx);
Lisp l_findFile(Lisp args, LispError *e, LispContext ctx);
Lisp l_projectRoot(Lisp args, LispError *e, LispContext ctx);
Lisp editorPromptLisp(Lisp args, LispError *e, LispContext ctx);

Lisp l_syntaxDefineLanguage(Lisp args, LispError *e, LispContext ctx);
Lisp l_syntaxAddKeyword(Lisp args, LispError *e, LispContext ctx);
Lisp l_syntaxAddType(Lisp args, LispError *e, LispContext ctx);

// Mouvement

Lisp moveCursor(Lisp args, LispError *e, LispContext ctx);
Lisp moveCursorBeginLine(Lisp args, LispError *e, LispContext ctx);
Lisp moveCursorEndLine(Lisp args, LispError *e, LispContext ctx);
Lisp editorMoveCursorPageUp(Lisp args, LispError* e, LispContext ctx);
Lisp editorMoveCursorPageDown(Lisp args, LispError *e, LispContext ctx);

// Text editing

Lisp l_editorInsertNewLine(Lisp args, LispError* e, LispContext ctx);
Lisp l_editorInserTab(Lisp args, LispError *e, LispContext ctx);
Lisp deletePreviousChar(Lisp args, LispError *e, LispContext ctx);
Lisp editorPrintC(Lisp args, LispError *e, LispContext ctx);
Lisp editorReadChar_L(Lisp args, LispError *e, LispContext ctx);

// Undo/redo — see bufferUndo()/bufferRedo() in buffer.c
Lisp editorUndo(Lisp args, LispError *e, LispContext ctx);
Lisp editorRedo(Lisp args, LispError *e, LispContext ctx);

// Selection (mark & region) — Emacs-style: set-mark, then move the cursor,
// then kill/copy the region. See bufferRegionBounds() in buffer.c.
Lisp editorSetMark(Lisp args, LispError *e, LispContext ctx);
Lisp editorKillRegion(Lisp args, LispError *e, LispContext ctx);
Lisp editorCopyRegion(Lisp args, LispError *e, LispContext ctx);

// Editor

Lisp editorQuit(Lisp args, LispError *e, LispContext ctx);
Lisp l_editorSave(Lisp args, LispError *e, LispContext ctx);
Lisp editorSetPrefix(Lisp args, LispError *e, LispContext ctx);
Lisp editorPrefix(Lisp args, LispError *e, LispContext ctx);
Lisp mapKey(Lisp args, LispError *e, LispContext ctx);
void registerBuiltin(const char * key_sequence, LispCFunc f);
Lisp editorOpenFile(Lisp args, LispError *e, LispContext ctx);
Lisp editorOpenConfig(Lisp args, LispError *e, LispContext ctx);

// Theme — (editor-set-theme-color role r g b), see src/builtins.c
Lisp editorSetThemeColor(Lisp args, LispError *e, LispContext ctx);
Lisp editorOpenFilePath(Lisp args, LispError *e, LispContext ctx);

// Directory listing / scratch buffers — file explorer, tree dump, etc.
Lisp l_listBuffers(Lisp args, LispError *e, LispContext ctx);
Lisp editorSwitchToBuffer(Lisp args, LispError *e, LispContext ctx);
Lisp editorKillBuffer(Lisp args, LispError *e, LispContext ctx); // (editor-kill-buffer [id])
Lisp editorPwd(Lisp args, LispError *e, LispContext ctx);
Lisp listDirectory(Lisp args, LispError *e, LispContext ctx);
Lisp editorScratchOpen(Lisp args, LispError *e, LispContext ctx);
Lisp editorScratchSetLines(Lisp args, LispError *e, LispContext ctx);
Lisp editorExplorerSidebarOpen(Lisp args, LispError *e, LispContext ctx);
Lisp editorExplorerClose(Lisp args, LispError *e, LispContext ctx);
Lisp addPackage(Lisp args, LispError *e, LispContext ctx);
Lisp editorOpenTerminal(Lisp args, LispError *e, LispContext ctx);
Lisp editorExecuteCommand(Lisp args, LispError *e, LispContext ctx);


// Buffer

Lisp editorSwitchNextBuffer(Lisp args, LispError *e, LispContext ctx);
Lisp editorSwitchBufferPopup(Lisp args, LispError *e, LispContext ctx);
Lisp bufferFind_L(Lisp args, LispError *e, LispContext ctx);
Lisp bufferFindReverse_L(Lisp args, LispError *e, LispContext ctx);


// Pane

Lisp l_editorSplitScreenVertical(Lisp args, LispError *e, LispContext ctx);
Lisp l_editorSplitScreenHorizontal(Lisp args, LispError *e, LispContext ctx);
Lisp editorSwitchNextPane(Lisp args, LispError *e, LispContext ctx);
Lisp editorUnifiedPanes(Lisp args, LispError *e, LispContext ctx);


Lisp editorPaste(Lisp args, LispError *e, LispContext ctx);
Lisp editorCutEndLine(Lisp args, LispError *e, LispContext ctx);
Lisp editorCopyEndLine(Lisp args, LispError *e, LispContext ctx);

Lisp editorMoveBegBuffer(Lisp args, LispError *e, LispContext ctx);
Lisp editorMoveEndBuffer(Lisp args, LispError *e, LispContext ctx);

// Other

Lisp editorAutoComplete(Lisp args, LispError *e, LispContext ctx);
// Lisp lspComplete(Lisp args, LispError *e, LispContext ctx);
Lisp lspDefinition(Lisp args, LispError *e, LispContext ctx);
Lisp lspHover(Lisp args, LispError *e, LispContext ctx);
Lisp lspRename(Lisp args, LispError *e, LispContext ctx);
Lisp lspFormatBuffer(Lisp args, LispError *e, LispContext ctx);

// (lsp-server "ext" "language-id" "command" "arg1" ...) — register which
// language server to spawn for files with this extension.
Lisp lspRegisterServer(Lisp args, LispError *e, LispContext ctx);

void bFree_structs(void);

// Hooks
//
// (add-hook "event-name" fn) registers fn (a lambda or function symbol —
// NOT the '(quoted-list) form used by map-key) to run whenever that event
// fires. runHooks() is what C code calls to fire one; see its call sites
// for the exact argument list each event passes. Currently fired events:
//   "buffer-opened" (filename, buffer-id) — a FILE_BUFF finished loading
//   "before-save"   (filename)            — about to write buffer to disk
//   "after-save"    (filename)            — buffer was just written
//   "lsp-ready"     ()                    — LSP handshake completed
//   "buffer-killed" (name, buffer-id)     — a buffer was just killed
//                                            (see bufferClose() in buffer.c)
Lisp addHook(Lisp args, LispError *e, LispContext ctx);
void runHooks(const char *event_name, Lisp args);

// Buffer/cursor introspection — lets Lisp code (hooks, custom commands,
// packages) make decisions based on real editor state instead of only
// being able to move the cursor blindly.
Lisp lineTextAt(Lisp args, LispError *e, LispContext ctx);        // (line-text [row])
Lisp cursorRowLisp(Lisp args, LispError *e, LispContext ctx);     // (cursor-row)
Lisp cursorColLisp(Lisp args, LispError *e, LispContext ctx);     // (cursor-col)
Lisp bufferLineCountLisp(Lisp args, LispError *e, LispContext ctx); // (buffer-line-count)
Lisp bufferFilenameLisp(Lisp args, LispError *e, LispContext ctx); // (buffer-filename)

// (editor-goto-line row [col]) — moves the cursor to row (0-based, clamped
// to the buffer), optionally to col (clamped to that row's length; 0 if
// omitted). Scrolling the row into view happens automatically on the next
// redraw (editorScroll() reconciles the cursor position every frame).
Lisp editorGotoLine(Lisp args, LispError *e, LispContext ctx);

// (editor-list-popup title items on-select [on-cancel]) — opens a generic
// modal picker. `items` is a list of (label . value) pairs; `on-select` is
// called with the chosen pair's cdr when the user presses ENTER;
// `on-cancel` (optional) is called with no arguments on ESC. See
// list_popup.h for the underlying C API this wraps.
Lisp editorListPopup(Lisp args, LispError *e, LispContext ctx);

// (app-debug?) — #t when the running binary was built with APP_DEBUG
// (install.sh --debug), #f otherwise. Always registered, so config/init.lisp
// can gate debug-only setup (e.g. the memory inspector below) without
// referencing a builtin that might not exist in this build.
Lisp l_appDebugP(Lisp args, LispError *e, LispContext ctx);

#ifdef APP_DEBUG
// (memory-inspector) — opens/refreshes the "*memory*" buffer listing every
// block gutils is currently tracking. See memory_inspector.h.
Lisp l_memoryInspectorOpen(Lisp args, LispError *e, LispContext ctx);
// (memory-inspector-refresh) — re-renders the snapshot in place.
Lisp l_memoryInspectorRefresh(Lisp args, LispError *e, LispContext ctx);
// (memory-inspector-sort) — cycles the display sort order and re-renders.
Lisp l_memoryInspectorCycleSort(Lisp args, LispError *e, LispContext ctx);
// (memory-inspector-export) — prompts for a path, writes the snapshot to it.
Lisp l_memoryInspectorExport(Lisp args, LispError *e, LispContext ctx);
#endif

#endif
