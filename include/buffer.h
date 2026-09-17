/**
 * @file buffer.h
 * @brief Buffer management for handling multiple open files
 */

#ifndef BUFFER_H_
#define BUFFER_H_

#include "data.h"

/**
 * @brief Creates a new buffer for a file
 * @param filename Path to the file
 * @return Buffer ID on success, -1 on failure
 */
int bufferCreate(const Utf8Char* path, enum bufferStatus_e state, enum buffer_type type);
/**
 * @brief Switches to a specific buffer by ID
 * @param buffer_id The buffer ID to switch to
 * @return 0 on success, -1 on failure
 */
int bufferSwitch(int buffer_id);

struct buffer_t *bufferFindById(int buffer_id);

/**
 * @brief Finds another FILE_BUFF buffer already LSP-synced for the same
 * file (by full path) as `buf` — see src/buffer.c for why this matters
 * now that the same file can be open in more than one buffer.
 * @return buffer_id of the other buffer, or -1 if none
 */
int bufferFindOtherLspOpen(struct buffer_t* buf);

/**
 * @brief Closes a buffer by ID
 * @param buffer_id The buffer ID to close
 * @return 0 on success, -1 on failure
 */
int bufferClose(int buffer_id);

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
char* bufferToText(struct buffer_t* buf);
void bufferMaybeStartLsp(struct buffer_t* buf);
void bufferMaybeSyncLsp(struct buffer_t* buf);
struct lspServerConfig_t* lspServerConfigFor(Utf8Char* filename);

// Is `cfg` the server that's actually running right now? "Only one
// language server runs at a time" (see bufferMaybeStartLsp()) means a
// buffer whose *own* registered extension maps to a different command
// than whatever's running must never be forwarded to it — sending, say,
// a .c file's content to a running digestif (a LaTeX assistant) tags it
// with digestif's languageId and gets nothing useful back at best, and at
// worst confuses the server's own state for the files it does understand.
// Compares by argv[0] rather than language_id, so several extensions that
// share one real server (typescript-language-server registered under
// "javascript"/"typescript"/... with a different language_id each) still
// correctly sync to it.
int lspServerConfigMatchesRunning(const struct lspServerConfig_t *cfg);

// ── Undo/redo ────────────────────────────────────────────────────────────
void bufferUndo(struct buffer_t* buf);
void bufferRedo(struct buffer_t* buf);

// ── Selection (mark & region) ───────────────────────────────────────────
// (start_row,start_col)/(end_row,end_col) must already be in document
// order (see bufferRegionBounds()).
void bufferRegionBounds(struct buffer_t* buf, int* start_row, int* start_col,
                        int* end_row, int* end_col);
char* bufferGetRegionText(struct buffer_t* buf, int start_row, int start_col,
                          int end_row, int end_col);
void bufferDeleteRegion(struct buffer_t* buf, int start_row, int start_col,
                        int end_row, int end_col);

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
