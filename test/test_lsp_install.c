/**
 * @file test_lsp_install.c
 * @brief Unit tests for (lsp-server-install ...): which command is offered
 * on which os, and in which order.
 */

#include "test.h"
#include "../include/lisp.h"
#include "../include/lisp_lib.h"
#include "../include/lsp_install.h"
#include "../include/os.h"

static LispContext ctx;

static LispError eval(const char *expr) {
  LispError error = LISP_ERROR_NONE;
  lisp_eval(lisp_read(expr, &error, ctx), &error, ctx);
  return error;
}

// A program that's on PATH everywhere the tests run, and one that isn't.
static const char *presentProgram(void) {
  return strcmp(os_name(), "windows") == 0 ? "cmd" : "sh";
}

static void testRegistry(void) {
  char expr[512];
  const char *needs;
  const char *have = presentProgram();

  // An os with no command, a os without its command: errors.
  CHECK(eval("(lsp-server-install \"srv\")") != LISP_ERROR_NONE);
  CHECK(eval("(lsp-server-install \"srv\" \"linux\")") != LISP_ERROR_NONE);
  CHECK(eval("(lsp-server-install \"srv\" \"linux\" 3)") != LISP_ERROR_NONE);

  // Nothing registered for it.
  CHECK(lspInstallCommandFor("nothing", "linux", &needs) == NULL);
  CHECK(needs == NULL);

  // Only commands whose program is missing: none offered, and the first
  // one's program is named.
  CHECK_INT(eval("(lsp-server-install \"srv\" \"linux\" \"sudo beluga-no-such-tool install srv\""
                 " \"any\" \"beluga-other-missing-tool srv\")"),
            LISP_ERROR_NONE);
  CHECK(lspInstallCommandFor("srv", "linux", &needs) == NULL);
  CHECK_STR(needs, "beluga-no-such-tool");
  CHECK(lspInstallCommandFor("srv", "windows", &needs) == NULL);
  CHECK_STR(needs, "beluga-other-missing-tool");

  // One that can run, after the missing ones: it's the one offered.
  snprintf(expr, sizeof(expr),
           "(lsp-server-install \"srv\" \"unix\" \"%s -c unix\" \"windows\" \"%s /c windows\")",
           have, have);
  CHECK_INT(eval(expr), LISP_ERROR_NONE);
  snprintf(expr, sizeof(expr), "%s -c unix", have);
  CHECK_STR(lspInstallCommandFor("srv", "linux", NULL), expr);
  CHECK_STR(lspInstallCommandFor("srv", "darwin", NULL), expr);
  snprintf(expr, sizeof(expr), "%s /c windows", have);
  CHECK_STR(lspInstallCommandFor("srv", "windows", NULL), expr);

  // A later call is tried first (personal-config over a package), its own
  // commands still in their order.
  snprintf(expr, sizeof(expr),
           "(lsp-server-install \"srv\" \"linux\" \"%s -c first\" \"linux\" \"%s -c second\")",
           have, have);
  CHECK_INT(eval(expr), LISP_ERROR_NONE);
  snprintf(expr, sizeof(expr), "%s -c first", have);
  CHECK_STR(lspInstallCommandFor("srv", "linux", NULL), expr);
  snprintf(expr, sizeof(expr), "%s -c unix", have);
  CHECK_STR(lspInstallCommandFor("srv", "darwin", NULL), expr);

  // Another server's commands don't leak into this one's.
  CHECK(lspInstallCommandFor("other", "linux", NULL) == NULL);
}

int main(void) {
  testInit();
  os_init();
  ctx = lisp_init();
  lisp_set_stderr(tmpfile(), ctx);
  lisp_lib_load(ctx);
  lisp_env_define(lisp_env(ctx), lisp_make_symbol("lsp-server-install", ctx),
                  lisp_make_func(lspRegisterInstall), ctx);
  testRegistry();
  lspInstallFree();
  lisp_shutdown(ctx);
  return testDone();
}
