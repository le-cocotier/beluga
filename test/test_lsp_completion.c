/**
 * @file test_lsp_completion.c
 * @brief Unit tests for filtering and ranking LSP completion items
 * (lspParseCompletion in lsp_ui.c)
 */

#include "test.h"
#include "../include/lsp_ui.h"

static CompletionPopup popup;

static void testMatchScore(void) {
  CHECK_INT(lspCompletionMatchScore("status", ""), 0);
  CHECK_INT(lspCompletionMatchScore("status", "st"), 0);
  CHECK_INT(lspCompletionMatchScore("Status", "st"), 1);
  CHECK_INT(lspCompletionMatchScore("getElementById", "gEBI"), 2);
  CHECK_INT(lspCompletionMatchScore("sendStatus", "sE"), 1);
  CHECK_INT(lspCompletionMatchScore("sendStatus", "sst"), 2);
  CHECK_INT(lspCompletionMatchScore("_construct", "st"), -1);
  CHECK_INT(lspCompletionMatchScore("json", "st"), -1);
  CHECK_INT(lspCompletionMatchScore("st", "status"), -1);
}

// What typescript-language-server sends for `res.st`: every member,
// unfiltered, members ranked by sortText.
static const char *tsResponse =
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"isIncomplete\":false,\"items\":["
    "{\"label\":\"_construct\",\"sortText\":\"11\",\"kind\":2},"
    "{\"label\":\"_destroy\",\"sortText\":\"11\",\"kind\":2},"
    "{\"label\":\"json\",\"sortText\":\"11\",\"kind\":2},"
    "{\"label\":\"statusMessage\",\"sortText\":\"11\",\"kind\":5},"
    "{\"label\":\"status\",\"sortText\":\"11\",\"kind\":2},"
    "{\"label\":\"setHeader\",\"sortText\":\"11\",\"kind\":2},"
    "{\"label\":\"sendStatus\",\"sortText\":\"11\",\"kind\":2},"
    "{\"label\":\"Stream\",\"sortText\":\"15\",\"kind\":7},"
    "{\"label\":\"stale\",\"filterText\":\"stale\",\"sortText\":\"10\",\"kind\":5,"
    "\"textEdit\":{\"range\":{\"start\":{\"line\":4,\"character\":6},"
    "\"end\":{\"line\":4,\"character\":8}},\"newText\":\"stale\"}}"
    "]}}";

static void testFiltersAndRanks(void) {
  lspParseCompletion(tsResponse, &popup, 0, 0, "st");
  CHECK_INT(popup.visible, 1);
  CHECK_INT(popup.count, 6);
  // exact-case prefixes first, by sortText then server order
  CHECK_STR(popup.items[0].label, "stale");
  CHECK_INT(popup.items[0].has_edit, 1);
  CHECK_STR(popup.items[1].label, "statusMessage");
  CHECK_STR(popup.items[2].label, "status");
  // then a case-insensitive prefix, then subsequences (s..t)
  CHECK_STR(popup.items[3].label, "Stream");
  CHECK_STR(popup.items[4].label, "setHeader");
  CHECK_STR(popup.items[5].label, "sendStatus");
}

static void testNothingTypedKeepsAll(void) {
  lspParseCompletion(tsResponse, &popup, 0, 0, "");
  CHECK_INT(popup.count, 9);
  CHECK_STR(popup.items[0].label, "stale"); // sortText "10"
  CHECK_STR(popup.items[1].label, "_construct");
  lspParseCompletion(tsResponse, &popup, 0, 0, NULL);
  CHECK_INT(popup.count, 9);
}

static void testNoMatch(void) {
  lspParseCompletion(tsResponse, &popup, 0, 0, "zz");
  CHECK_INT(popup.count, 0);
  CHECK_INT(popup.visible, 0);
}

// More items than the popup holds: the best ones are kept, not the first.
static void testKeepsBestWhenTruncating(void) {
  char json[8192];
  size_t n;
  int i;
  n = (size_t)snprintf(json, sizeof(json),
                       "{\"id\":1,\"result\":[");
  for (i = 0; i < 40; i++)
    n += (size_t)snprintf(json + n, sizeof(json) - n,
                          "{\"label\":\"aaa%02d\",\"sortText\":\"1\"},", i);
  snprintf(json + n, sizeof(json) - n,
           "{\"label\":\"wanted\",\"sortText\":\"1\"}]}");
  lspParseCompletion(json, &popup, 0, 0, "wa");
  CHECK_INT(popup.count, 1);
  CHECK_STR(popup.items[0].label, "wanted");
  lspParseCompletion(json, &popup, 0, 0, "");
  CHECK_INT(popup.count, COMPLETION_MAX_ITEMS);
}

// clangd: already filtered, ranked by sortText, labels with a leading
// space or "•".
static void testClangdOrderKept(void) {
  lspParseCompletion(
      "{\"id\":2,\"result\":{\"isIncomplete\":true,\"items\":["
      "{\"label\":\" printf(const char *, ...)\",\"filterText\":\"printf\",\"sortText\":\"3f\"},"
      "{\"label\":\"\xe2\x80\xa2pread(int, void *)\",\"filterText\":\"pread\",\"sortText\":\"40\"},"
      "{\"label\":\" prf_t\",\"filterText\":\"prf_t\",\"sortText\":\"2a\"}]}}",
      &popup, 0, 0, "pr");
  CHECK_INT(popup.count, 3);
  CHECK_STR(popup.items[0].label, "prf_t");
  CHECK_STR(popup.items[1].label, "printf(const char *, ...)");
  CHECK_STR(popup.items[2].label, "pread(int, void *)");
}

int main(void) {
  testInit();
  testMatchScore();
  testFiltersAndRanks();
  testNothingTypedKeepsAll();
  testNoMatch();
  testKeepsBestWhenTruncating();
  testClangdOrderKept();
  return testDone();
}
