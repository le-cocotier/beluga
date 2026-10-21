//
// Created by Giorgio on 27/05/2026.
//

#ifndef BELUGA_LSP_UI_H
#define BELUGA_LSP_UI_H


#include "data.h"
#include "buffer.h"   // for abuf

// ── Public API ────────────────────────────────────────────────────────────────

// Call from editorRefreshScreen() — appends popup/underlines to abuf
void lspUiDrawCompletion(struct abuf *ab, CompletionPopup *popup);
void lspUiDrawDiagnostics(struct abuf *ab, DiagnosticList *diags,
                          EditorPane *pane, struct buffer_t *buf);

// Input handling — returns 1 if the key was consumed by the popup
int lspUiHandleKey(CompletionPopup *popup, const Utf8Char *key);

// Parse clangd JSON into our structs
void lspParseCompletion(const char *json, CompletionPopup *popup,
                        int screen_x, int screen_y);
void lspParseDiagnostics(const char *json, DiagnosticList *diags, int buffer_id);

// git_status is drawn when there's no diagnostic on this line — pass
// GIT_LINE_NONE (or the buffer's git_lines[line] when available) if git
// info doesn't apply.
void lspUiDrawGutter(struct abuf *ab, DiagnosticList *diags,
                     int buf_id, int line, GitLineStatus git_status);
// Hover / signature help popup. lspUiSetInfo() replaces its text (markdown
// fences, headings and backticks stripped) and shows it; anchor_x/y are the
// cursor's 0-based screen position. lspUiInfoHandleKey() returns 1 only for
// ESC — every other key still reaches the editor.
void lspUiSetInfo(LspInfoPopup *popup, LspInfoKind kind, const char *text,
                  int anchor_x, int anchor_y);
void lspUiDrawInfo(struct abuf *ab, const LspInfoPopup *popup);
int lspUiInfoHandleKey(LspInfoPopup *popup, const Utf8Char *key);
const char *lspUiDiagnosticAtCursor(DiagnosticList *diags,
                                    int buffer_id, int cursor_line);

// How many bytes of `s` (at most nbytes) fit in max_cols screen columns,
// never splitting a character; *cols gets the columns they take.
int lspUiClipToColumns(const char *s, int nbytes, int max_cols, int *cols);
// The most severe diagnostic on `line` of the buffer, or NULL.
const Diagnostic *lspUiWorstDiagnostic(const DiagnosticList *diags, int buffer_id, int line);
// Draws "   ● message" in the severity's color, clipped to max_cols (nothing
// when that's too narrow to read). @return the columns drawn.
int lspUiDrawInlineDiagnostic(struct abuf *ab, const Diagnostic *d, int max_cols);
// Draws "   // text" in the comment color (a struct layout note), clipped to
// max_cols. @return the columns drawn.
int lspUiDrawEolNote(struct abuf *ab, const char *text, int max_cols);
// The functions, structs, enums, ... whose body contains `row`, outermost
// first, from the buffer's background-refreshed LSP symbols. @return how many.
int lspUiEnclosingSymbols(const struct buffer_t *buf, int row,
                          const LspSymbol **out, int max);
// Their names joined with " › " ("point › main"). @return its length in bytes.
int lspUiBreadcrumbs(const struct buffer_t *buf, int row, char *out, size_t size);


#endif
