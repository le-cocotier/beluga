/**
 * @file buffer.h
 * @brief Buffer management for handling multiple open files
 */

#ifndef BUFFER_H_
#define BUFFER_H_

#include "data.h"

/**
 * @brief Creates a new buffer for a file
 * @param path Path to the file
 * @param state READ_ONLY or READ_AND_WRITE
 * @param type FILE_BUFF, TERMINAL_BUFF, or EXPLORER_BUFF
 * @return Buffer ID on success, -1 on failure
 */
int bufferCreate(const Utf8Char* path, enum bufferStatus_e state, enum buffer_type type);
/**
 * @brief Switches to a specific buffer by ID
 * @param buff_id The buffer ID to switch to
 * @return 0 on success, -1 on failure
 */
int bufferSwitch(int buff_id);

struct buffer_t *bufferFindById(int buff_id);

/**
 * @brief Finds another FILE_BUFF buffer already LSP-synced for the same
 * file (by full path) as `buf` — see src/buffer.c for why this matters
 * now that the same file can be open in more than one buffer.
 * @return buffer_id of the other buffer, or -1 if none
 */
int bufferFindOtherLspOpen(struct buffer_t* buf);

/**
 * @brief Kills a buffer: panes showing it switch to another buffer, then
 * it's freed. Asks first for a modified file or a live terminal; refuses
 * to kill the last buffer. Fires the "buffer-killed" hook (name buffer-id).
 * @param buff_id The buffer ID to kill
 * @return 0 on success, -1 if not found, refused, or cancelled
 */
int bufferClose(int buff_id);

/**
 * @brief Gets the current active buffer
 * @return Pointer to current buffer_t, NULL if none
 */
struct buffer_t *bufferGetCurrent(void);

/**
 * @brief Lists all open buffers
 * @return Number of open buffers
 */
int bufferListAll(void);

/**
 * @brief Saves current buffer to disk
 * @return 0 on success, -1 on failure
 */
int bufferSave(void);

/**
 * @brief Saves all buffers to disk
 * @return 0 on success, -1 on failure
 */
int bufferSaveAll(void);

void bufferFind(struct buffer_t *buf);
void bufferFindReverse(struct buffer_t* buf);
int bufferFindByFilename(Utf8Char *filename);
void bufferInsertNewLine();
void bufferInsertChar(Utf8Char * character);
void bufferDelChar();
void bufferRowInsertChar(struct buffer_t *buffer, Row *row, int at, Utf8Char *character);
void bufferRowDelChar(struct buffer_t *buffer, Row *row, int at);
void bufferInsertRow(struct buffer_t *buffer, int at,  Utf8Char* string, int len);
void bufferFreeRow(Row *row);
// Frees an (already unlinked) buffer and everything it owns.
void bufferFree(struct buffer_t *buf);
char* bufferToText(struct buffer_t* buf);
void bufferMaybeStartLsp(struct buffer_t* buf);
void bufferMaybeSyncLsp(struct buffer_t* buf);
struct lspServerConfig_t* lspServerConfigFor(Utf8Char* filename);

// The LSP client for a server config / a buffer's file / the active pane's
// buffer: one per server command, created the first time a matching file
// is opened. NULL when there is none (no server registered for it, or it
// was never asked about). Check ->state before sending anything.
LspClient *lspClientForConfig(const struct lspServerConfig_t *cfg);
LspClient *lspClientForBuffer(struct buffer_t *buf);
LspClient *lspActiveClient(void);

// ── Undo/redo ────────────────────────────────────────────────────────────
void bufferUndo(struct buffer_t* buf);
void bufferRedo(struct buffer_t* buf);
// Brackets a command made of many edits (a query-replace, say) so that one
// undo reverts all of it. Not nested.
void bufferUndoGroupBegin(struct buffer_t* buf);
void bufferUndoGroupEnd(struct buffer_t* buf);

// ── Selection (mark & region) ───────────────────────────────────────────
// (start_row,start_col)/(end_row,end_col) must already be in document
// order (see bufferRegionBounds()).
void bufferRegionBounds(struct buffer_t* buf, int* start_row, int* start_col,
                        int* end_row, int* end_col);
char* bufferGetRegionText(struct buffer_t* buf, int start_row, int start_col,
                          int end_row, int end_col);
void bufferDeleteRegion(struct buffer_t* buf, int start_row, int start_col,
                        int end_row, int end_col);
// Inserts `text` (may contain '\n') at the cursor of `buf` as one undo
// step, leaving the cursor after it. Used by paste.
void bufferInsertText(struct buffer_t* buf, const char* text);
// Drops the undo and redo history, e.g. when the content is replaced.
void bufferClearUndo(struct buffer_t* buf);

// ── Generic text-edit application (LSP formatting/rename use this) ──────
// Replaces the text in [start_row:start_col, end_row:end_col) with
// new_text (may contain '\n' and be any length, including empty — a pure
// deletion). Goes through the same undo-recorded primitives as normal
// typing, so LSP edits are undoable like anything else.
void bufferApplyTextEdit(struct buffer_t* buf, int start_row, int start_col,
                         int end_row, int end_col, const char* new_text);

// ── Git integration ──────────────────────────────────────────────────────
void bufferRefreshGitInfo(struct buffer_t* buf);

#endif
