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

#endif
