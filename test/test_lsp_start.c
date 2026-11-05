/**
 * @file test_lsp_start.c
 * @brief Unit tests for starting a language server (lspStart in
 * completion.c): a server that answers `initialize` with an error.
 */

#include "test.h"
#include "../include/completion.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/utf8.h"

// What typescript-language-server answers when the only TypeScript it can
// find is one it doesn't accept.
static const char *initError =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":"
    "\"Request initialize failed with message: Could not find a valid "
    "TypeScript installation.\"}}";

// Polls `lsp` like the main loop does, until its process is released.
static void pollUntilExited(LspClient *lsp) {
  int waited = 0;
  while (lsp->proc && waited < 5000) {
    lspPoll(lsp);
    os_sleep_ms(10);
    waited += 10;
  }
}

static void testInitializeError(void) {
  LspClient *lsp;
  char script[512];
  char *argv[4];
  char *status;

  // A "server" that answers the initialize request with that error, then
  // keeps reading its stdin: only beluga killing it ends it.
  snprintf(script, sizeof(script),
           "printf 'Content-Length: %d\\r\\n\\r\\n%%s' '%s'; exec cat >/dev/null",
           (int)strlen(initError), initError);
  argv[0] = "sh";
  argv[1] = "-c";
  argv[2] = script;
  argv[3] = NULL;

  lsp = (LspClient *)gcalloc(1, sizeof(LspClient));
  strcpy(lsp->command, "typescript-language-server");
  CHECK_INT(lspStart(lsp, "/", argv), 0);
  CHECK_INT(lsp->state, LSP_INITIALIZING);
  pollUntilExited(lsp);

  // Not READY, the process gone, and the reason kept and shown.
  CHECK_INT(lsp->state, LSP_SHUTDOWN);
  CHECK(lsp->proc == NULL);
  CHECK_STR(lsp->start_error, "Could not find a valid TypeScript installation.");
  status = (char *)utf8ToString(E.status_msg);
  CHECK_STR(status, "typescript-language-server failed to start: "
                    "Could not find a valid TypeScript installation.");
  gfree(status);

  jsonRpcStreamFree(&lsp->recv);
  gfree(lsp);
}

int main(void) {
  testInit();
  // The fake server is a POSIX shell script.
  if (strcmp(os_name(), "windows") != 0)
    testInitializeError();
  return testDone();
}
