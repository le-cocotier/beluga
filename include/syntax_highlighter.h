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
  TOKEN_DEFAULT,
  TOKEN_ADDED,
  TOKEN_REMOVED,
  // Attributes rather than colors: the text keeps the default color and
  // is drawn bold or italic. For syntax-add-span (Markdown's **x** and *x*).
  TOKEN_BOLD,
  TOKEN_ITALIC
} TokenType;

// Finds the LanguageSyntax registered for `filename`'s extension (see the
// Lisp `syntax-define-language` builtin), or NULL if none was registered —
// which is exactly what a file gets rendered as plain text, no highlight_line()
// call at all. There is no built-in fallback language: even C's highlighting
// comes entirely from what the "c" package registers, the same as every
// other language.
const LanguageSyntax *syntaxLanguageFor(const Utf8Char *filename);
const LanguageSyntax *syntaxLanguageForExtension(const char *ext);
// The buffer's language: its buffer-set-syntax override if any, else by
// filename.
const LanguageSyntax *syntaxLanguageForBuffer(const Buffer *buf);
// "keyword", "type", "string", "comment", "number", "added", "removed",
// "default", "bold", "italic" -> TokenType, or -1.
int syntaxRoleFromName(const char *name);

// Highlights one row according to `lang`'s rules, clipped to the pane's
// visible window like the plain-text row path. `lang` must be non-NULL —
// callers only reach this once syntaxLanguageFor() has already found one.
char *highlight_line(const Row *row, int *length, int x_offset, int width,
                     const LanguageSyntax *lang);
int is_word_char(const Utf8Char *c);

// Inside a comment of an `ext` file, draws `word` in `role`'s color and,
// when next_role >= 0, the word after it in next_role's (a TokenType each).
// Independent of the language being defined yet. 0 if `word` or `ext` is
// too long (31 / 15 bytes) or out of memory.
int syntaxAddCommentWord(const char *ext, const char *word, int role, int next_role);
void syntaxFreeCommentWords(void);

// ── Block-comment state cache ──────────────────────────────────────────────
// See the comment_state fields on struct buffer_t (data.h) for why this
// exists: highlight_line() only ever sees the rows currently on screen, so
// after a scroll it has no way to know whether the first visible row starts
// inside a block comment that opened somewhere above it.

// Returns 1 if `row` starts inside an unclosed block comment, computed (and
// cached) by scanning the whole buffer from the top using the buffer's own
// registered language's comment delimiters. Always 0 for a buffer with no
// registered language, or one whose language has no block comment at all
// (e.g. YAML) — there's nothing to track in either case. Call this once per
// pane, right before rendering its first visible row, and seed the
// highlighter's carry-over state from the result.
int commentStateAtRowStart(struct buffer_t *buf, int row);

// Marks every row after `row` stale — an edit to row N can only change
// whether rows N+1.. start inside a comment, never N or anything above it.
// Call after any edit to row `row`, or after inserting/removing a row at
// `row` (pass `row - 1` there: the shifted rows from `row` on are stale).
void syntaxHighlighterInvalidateAfter(struct buffer_t *buf, int row);

// Seeds highlight_line()'s cross-call comment_section state. Call once per
// pane, right before its first visible row, with commentStateAtRowStart()'s
// result for that row — a thin setter rather than exposing comment_section
// itself, so callers outside this file go through one clearly-named entry
// point instead of reaching into an internal global directly.
void syntaxHighlighterSeed(int in_comment);

#endif
