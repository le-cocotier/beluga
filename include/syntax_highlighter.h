#ifndef SYNTAX_HIGHLIGHTER_H_
#define SYNTAX_HIGHLIGHTER_H_

#include "data.h"

// Color codes that only affect foreground, preserving your background color
#define COLOR_RESET         "\x1b[39m"            // Reset all (4 bytes)

// Token types
typedef enum {
  TOKEN_KEYWORD,
  TOKEN_TYPE,
  TOKEN_STRING,
  TOKEN_COMMENT,
  TOKEN_NUMBER,
  TOKEN_OPERATOR,
  TOKEN_DEFAULT
} TokenType;

char *highlight_line(const Row *row, int *length, int x_offset, int width);
int is_word_char(const Utf8Char *c);

// ── Block-comment state cache ──────────────────────────────────────────────
// See the comment_state fields on struct buffer_t (data.h) for why this
// exists: highlight_line() only ever sees the rows currently on screen, so
// after a scroll it has no way to know whether the first visible row starts
// inside a "/* ... */" that opened somewhere above it.

// Returns 1 if `row` starts inside an unclosed block comment, computed (and
// cached) by scanning the whole buffer from the top. Call this once per
// pane, right before rendering its first visible row, and seed the
// highlighter's carry-over state from the result.
int commentStateAtRowStart(struct buffer_t *buf, int row);

// Marks the cache stale, so the next commentStateAtRowStart() call rebuilds
// it. Call after any edit to the buffer.
void syntaxHighlighterInvalidate(struct buffer_t *buf);

// Seeds highlight_line()'s cross-call comment_section state. Call once per
// pane, right before its first visible row, with commentStateAtRowStart()'s
// result for that row — a thin setter rather than exposing comment_section
// itself, so callers outside this file go through one clearly-named entry
// point instead of reaching into an internal global directly.
void syntaxHighlighterSeed(int in_comment);

#endif
