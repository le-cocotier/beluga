/**
 * @file test_lsp_symbols.c
 * @brief Unit tests for parsing documentSymbol / workspace/symbol answers
 * (completion.c), which the outline and symbol search are built on
 */

#include "test.h"
#include "../include/completion.h"

// What clangd sends for documentSymbol: a tree, landing on the name
// (selectionRange) rather than on the doc comment (range).
static void testDocumentSymbolTree(void) {
  LspSymbol *s;
  const int n = lspParseSymbols(
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":["
      "{\"name\":\"point\",\"kind\":23,"
      "\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":5,\"character\":1}},"
      "\"selectionRange\":{\"start\":{\"line\":3,\"character\":7},\"end\":{\"line\":3,\"character\":12}},"
      "\"children\":["
      "{\"name\":\"x\",\"kind\":8,\"detail\":\"int\","
      "\"range\":{\"start\":{\"line\":4,\"character\":2},\"end\":{\"line\":4,\"character\":7}},"
      "\"selectionRange\":{\"start\":{\"line\":4,\"character\":6},\"end\":{\"line\":4,\"character\":7}}}]},"
      "{\"name\":\"main\",\"kind\":12,\"detail\":\"int (void)\","
      "\"range\":{\"start\":{\"line\":7,\"character\":0},\"end\":{\"line\":9,\"character\":1}},"
      "\"selectionRange\":{\"start\":{\"line\":7,\"character\":4},\"end\":{\"line\":7,\"character\":8}}}"
      "]}",
      &s);
  CHECK_INT(n, 3);
  if (n != 3)
    return;
  CHECK_STR(s[0].name, "point");
  CHECK_INT(s[0].line, 3);
  CHECK_INT(s[0].col, 7);
  CHECK_INT(s[0].depth, 0);
  CHECK_INT(s[0].start_line, 2); // the whole range: sticky scroll, breadcrumbs
  CHECK_INT(s[0].end_line, 5);
  CHECK_STR(lspSymbolKindName(s[0].kind), "struct");
  CHECK_STR(s[1].name, "x");
  CHECK_INT(s[1].depth, 1);
  CHECK_STR(s[1].detail, "int");
  CHECK_STR(s[2].name, "main");
  CHECK_STR(lspSymbolKindName(s[2].kind), "function");
  CHECK(s[2].path == NULL);
  lspFreeSymbols(s, n);
}

// What workspace/symbol sends: flat, each with its file.
static void testSymbolInformation(void) {
  LspSymbol *s;
  const int n = lspParseSymbols(
      "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":["
      "{\"name\":\"bufferCreate\",\"kind\":12,\"containerName\":\"\","
      "\"location\":{\"uri\":\"file:///src/buffer.c\","
      "\"range\":{\"start\":{\"line\":41,\"character\":4},\"end\":{\"line\":41,\"character\":16}}}},"
      "{\"name\":\"numrows\",\"kind\":8,\"containerName\":\"buffer_t\","
      "\"location\":{\"uri\":\"file:///include/data.h\","
      "\"range\":{\"start\":{\"line\":9,\"character\":8},\"end\":{\"line\":9,\"character\":15}}}},"
      "{\"name\":\"remote\",\"kind\":12,"
      "\"location\":{\"uri\":\"https://example.com/x.c\","
      "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}}}}"
      "]}",
      &s);
  CHECK_INT(n, 2); // the non-file URI is skipped
  if (n != 2)
    return;
  CHECK_STR(s[0].name, "bufferCreate");
  CHECK(s[0].detail == NULL); // an empty container is no container
  CHECK(s[0].path != NULL && strstr(s[0].path, "buffer.c") != NULL);
  CHECK_INT(s[0].line, 41);
  CHECK_STR(s[1].detail, "buffer_t");
  CHECK_STR(lspSymbolKindName(s[1].kind), "field");
  lspFreeSymbols(s, n);
}

static void testEmptyAndErrors(void) {
  LspSymbol *s;
  CHECK_INT(lspParseSymbols("{\"id\":1,\"result\":[]}", &s), 0);
  CHECK_INT(lspParseSymbols("{\"id\":1,\"result\":null}", &s), 0);
  CHECK_INT(lspParseSymbols(
                "{\"id\":1,\"error\":{\"code\":-32601,\"message\":\"no\"}}", &s),
            -1);
  CHECK_INT(lspParseSymbols("not json", &s), -1);
  CHECK(s == NULL);
  CHECK_STR(lspSymbolKindName(0), "symbol");
  CHECK_STR(lspSymbolKindName(99), "symbol");
}

// Labels as strings or as parts, with their padding; sorted by position.
static void testInlayHints(void) {
  LspInlayHint *h;
  const int n = lspParseInlayHints(
      "{\"id\":3,\"result\":["
      "{\"position\":{\"line\":4,\"character\":9},\"label\":\"b:\",\"paddingRight\":true},"
      "{\"position\":{\"line\":4,\"character\":6},\"label\":[{\"value\":\"a\"},{\"value\":\":\"}],"
      "\"paddingRight\":true},"
      "{\"position\":{\"line\":1,\"character\":3},\"label\":\"int\",\"paddingLeft\":true},"
      "{\"position\":{\"line\":2},\"label\":\"broken\"},"
      "{\"position\":{\"line\":2,\"character\":0},\"label\":\"a\\nb\"}"
      "]}",
      &h);
  CHECK_INT(n, 4);
  if (n != 4)
    return;
  CHECK_INT(h[0].line, 1);
  CHECK_STR(h[0].label, " int");
  CHECK_STR(h[1].label, "a b"); // no newline into the frame
  CHECK_INT(h[2].col, 6);
  CHECK_STR(h[2].label, "a: ");
  CHECK_INT(h[3].col, 9);
  CHECK_STR(h[3].label, "b: ");
  lspFreeInlayHints(h, n);
  CHECK_INT(lspParseInlayHints("{\"id\":1,\"result\":null}", &h), 0);
  lspFreeInlayHints(h, 0);
  CHECK_INT(lspParseInlayHints("{\"id\":1,\"error\":{}}", &h), -1);
}

int main(void) {
  testInit();
  testInlayHints();
  testDocumentSymbolTree();
  testSymbolInformation();
  testEmptyAndErrors();
  return testDone();
}
