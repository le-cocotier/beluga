#ifndef BELUGA_COMPLETION_H
#define BELUGA_COMPLETION_H
#include "data.h"

// ── Lifecycle ────────────────────────────────────────────────────────────────
// server_argv is a NULL-terminated argv for the language server process,
// e.g. (char*[]){"clangd", NULL} or {"pyright-langserver", "--stdio", NULL}.
int lspStart(LspClient* lsp, const char* project_root, char* const server_argv[]);
void lspShutdown(LspClient* lsp);
void lspCleanup(LspClient* lsp);

// ── Non-blocking I/O — call from editorRunLoop() when the server's output
// stream is ready ───────────────────────────────────────────────────────────
void lspPoll(LspClient* lsp);

// ── Message dispatch (internal to completion.c's lspPoll, but exposed
// in case lsp_ui.c or tests need to feed a message directly) ────────────────
void lspHandleMessage(LspClient* lsp, const char* json);

// ── Document sync — call these from your buffer open/save/edit hooks ────────
void lspDidOpen(LspClient* lsp, struct buffer_t* buf);
void lspDidChange(LspClient* lsp, struct buffer_t* buf);
void lspDidClose(LspClient* lsp, struct buffer_t* buf);

// ── Requests ─────────────────────────────────────────────────────────────────
void lspRequestCompletion(LspClient* lsp, const struct buffer_t* buf,
                          int line, int col,
                          int screen_x, int screen_y);
// Response opens a popup below the cursor (E.lsp_info); any key closes it.
void lspRequestHover(LspClient* lsp, struct buffer_t* buf, int line, int col);
// Response opens a popup above the cursor (E.lsp_info) with the active
// parameter highlighted; it stays up while typing the arguments.
void lspRequestSignatureHelp(LspClient* lsp, struct buffer_t* buf, int line, int col);
// Asks for the code actions at the cursor — passing the server's own
// diagnostics for its line, so their quick fixes come back too — and lists
// them in a popup.
void lspRequestCodeAction(LspClient* lsp, struct buffer_t* buf, int line, int col);
// Applies entry `index` of the last code-action list: its edit, then its
// command (workspace/executeCommand), if it has either.
void lspApplyCodeAction(LspClient* lsp, int index);
// Accepting a completion: replaces the item's textEdit range — the
// already-typed prefix — or, without one, the word before the cursor.
void lspAcceptCompletion(struct buffer_t* buf, const CompletionItem* item);
// Response jumps the cursor to the definition, opening the target file
// first if it isn't already open — see lspJumpToUri() in completion.c.
void lspRequestDefinition(LspClient* lsp, struct buffer_t* buf, int line, int col);
// textDocument/references: the answer is listed in the "*search*" buffer
// (ENTER jumps), the declaration included.
void lspRequestReferences(LspClient* lsp, struct buffer_t* buf, int line, int col);
// Every diagnostic of the open buffers, listed the same way.
void lspShowDiagnosticsList(void);
// Prompts aren't this function's job — pass the already-chosen new name.
// Response applies the server's WorkspaceEdit (both the "changes" and
// "documentChanges" shapes) across every affected buffer, opening files
// that aren't currently open, and marks each one dirty without saving —
// the user reviews/saves them like any other edit.
void lspRequestRename(LspClient* lsp, struct buffer_t* buf, int line, int col,
                      const char* new_name);
// Response applies the returned TextEdit[] to `buf` specifically (the
// request itself carries no file info of its own to recover it from).
void lspRequestFormatting(LspClient* lsp, struct buffer_t* buf);

// Blocks (pumping lsp's own fd only — nothing else in the editor runs
// meanwhile) until the response for `request_id` has been dispatched, or
// timeout_ms elapses. Used by format-on-save and rename to get a
// synchronous result out of what's otherwise an async client — safe only
// because nothing else can be in flight for the *same* buffer while this
// blocks (see the callers in builtins.c).
// @return 1 if the response arrived, 0 on timeout.
int lspAwaitResponse(LspClient* lsp, int request_id, int timeout_ms);

// ── Completion debounce ──────────────────────────────────────────────────────
// Call lspScheduleCompletion() from the input path instead of requesting a
// completion directly on every keystroke. It (re)starts a short idle timer;
// lspCompletionDebounceTick() — called once per editorRunLoop iteration —
// actually fires the didChange+completion round-trip once typing pauses.
// lspCancelScheduledCompletion() drops a pending request that's no longer
// relevant (e.g. the user moved on to a non-completion-triggering key).
void lspScheduleCompletion(void);
void lspCancelScheduledCompletion(void);
void lspCompletionDebounceTick(void);

// ── Document sync debounce ───────────────────────────────────────────────────
// Needs no schedule call from the input path — call once per editorRunLoop
// iteration, alongside lspCompletionDebounceTick(). It notices ANY edit to
// the active buffer (by comparing buf->dirty to buf->lsp_synced_dirty) and
// resyncs it to the server shortly after edits stop, independent of whether
// that edit would also have triggered completion. See its doc comment in
// completion.c for why this exists separately from the completion debounce.
void lspSyncDebounceTick(void);

#endif //BELUGA_COMPLETION_H
