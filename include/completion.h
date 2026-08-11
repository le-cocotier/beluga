#ifndef BELUGA_COMPLETION_H
#define BELUGA_COMPLETION_H
#include "data.h"

// ── Lifecycle ────────────────────────────────────────────────────────────────
// server_argv is a NULL-terminated argv for the language server process,
// e.g. (char*[]){"clangd", NULL} or {"pyright-langserver", "--stdio", NULL}.
int lspStart(LspClient* lsp, const char* project_root, char* const server_argv[]);
void lspShutdown(LspClient* lsp);
void lspCleanup(LspClient* lsp);

// ── Non-blocking I/O — call from editorRunLoop's select() when
// lsp->read_from_server_fd is readable ─────────────────────────────────────
void lspPoll(LspClient* lsp);

// ── Message dispatch (internal to completion.c's lspPoll, but exposed
// in case lsp_ui.c or tests need to feed a message directly) ────────────────
void lspHandleMessage(LspClient* lsp, const char* json);

// ── Document sync — call these from your buffer open/save/edit hooks ────────
void lspDidOpen(LspClient* lsp, struct buffer_t* buf);
void lspDidChange(LspClient* lsp, struct buffer_t* buf);
void lspDidClose(LspClient* lsp, struct buffer_t* buf);

// ── Requests ─────────────────────────────────────────────────────────────────
void lspRequestCompletion(LspClient* lsp, struct buffer_t* buf,
                          int line, int col,
                          int screen_x, int screen_y);
void lspRequestHover(LspClient* lsp, struct buffer_t* buf, int line, int col); // not yet implemented
void lspRequestDefinition(LspClient* lsp, struct buffer_t* buf, int line, int col);

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

#endif //BELUGA_COMPLETION_H
