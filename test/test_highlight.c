/**
 * @file test_highlight.c
 * @brief Unit tests for syntax_highlighter.c: words colored inside comments
 * (syntax-add-comment-word)
 */

#include "test.h"
#include "../include/buffer.h"
#include "../include/galloc.h"
#include "../include/syntax_highlighter.h"

// Distinct, readable stand-ins for the theme's escape sequences.
#define KW "<kw>"
#define TY "<ty>"
#define CM "<cm>"
#define DF "<df>"

extern EditorConfig E;

static LanguageSyntax lang;

// The highlighted row with the reset sequence taken out.
static char *render(const char *text) {
  Buffer *buf = testBufferFromText(text);
  int len;
  char *out = highlight_line(&buf->row[0], &len, 0, 200, &lang);
  char *src;
  char *dst;
  const char *reset = COLOR_RESET;
  bufferFree(buf);
  for (src = dst = out; *src;) {
    if (!strncmp(src, reset, strlen(reset)))
      src += strlen(reset);
    else
      *dst++ = *src++;
  }
  *dst = '\0';
  return out;
}

static void check(const char *text, const char *expected) {
  char *out = render(text);
  CHECK_STR(out, expected);
  gfree(out);
}

int main(void) {
  testInit();
  E.theme.COLOR_KEYWORD = (char *)KW;
  E.theme.COLOR_TYPE = (char *)TY;
  E.theme.COLOR_COMMENT = (char *)CM;
  E.theme.COLOR_DEFAULT = (char *)DF;
  strcpy(lang.extension, "c");
  strcpy(lang.line_comment, "//");
  strcpy(lang.block_comment_open, "/*");
  strcpy(lang.block_comment_close, "*/");

  // Nothing registered: one comment color.
  syntaxHighlighterSeed(0);
  check("/* @brief x */", CM "/* @brief x */");

  CHECK_INT(syntaxAddCommentWord("c", "@brief", TOKEN_KEYWORD, -1), 1);
  CHECK_INT(syntaxAddCommentWord("c", "@param", TOKEN_KEYWORD, TOKEN_TYPE), 1);
  CHECK_INT(syntaxAddCommentWord("h", "@return", TOKEN_KEYWORD, -1), 1);

  syntaxHighlighterSeed(0);
  check("/* @brief Adds. */", CM "/* " KW "@brief" CM " Adds. */");
  // Continuing a block comment, and the word after @param.
  syntaxHighlighterSeed(1);
  check(" * @param count how many", " " CM "* " KW "@param" " " TY "count" CM " how many");
  syntaxHighlighterSeed(1);
  check(" * @param[in] n */", " " CM "* " KW "@param[in]" " " TY "n" CM " */");
  // Line comments too; whole words only; another extension's words aren't.
  syntaxHighlighterSeed(0);
  check("// @brief @briefly @return", CM "// " KW "@brief" CM " @briefly @return");
  syntaxHighlighterSeed(0);
  check("/* @param */", CM "/* " KW "@param" " " TY CM "*/");

  syntaxFreeCommentWords();
  return testDone();
}
