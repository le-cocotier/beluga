#ifndef BELUGA_LSP_INSTALL_H
#define BELUGA_LSP_INSTALL_H

#include "data.h"
#include "lisp.h"

// ─── Installing a missing language server ───────────────────────────────────
// (lsp-server-install "command" "os" "shell command" ["os" "shell command"]...)
// tells beluga how to install a language server on each platform. When a
// file whose server isn't on PATH is opened, the user is asked whether to
// run that command; it runs in a terminal pane, *lsp-install*, so its
// progress shows and a sudo password or a licence question can be answered.
// Once it ends and the server is found, the usual "Start the ... language
// server?" question follows.

// The Lisp builtin, registered as lsp-server-install.
Lisp lspRegisterInstall(Lisp args, LispError *e, LispContext ctx);

/**
 * @brief Called by bufferMaybeStartLsp() before it asks to start `cfg`'s
 * server for `buf`.
 * @return 0 when the server is installed: go on and ask to start it.
 *         1 when it isn't: the user was offered to install it (or told how
 *         to), and the start is up to lspInstallTick() once it's installed.
 */
int lspInstallOffer(struct buffer_t *buf, const struct lspServerConfig_t *cfg);

// Starts an accepted install, and notices when it has ended. Every tick.
void lspInstallTick(void);

// Releases the registrations (at exit, with the rest of the config).
void lspInstallFree(void);

/**
 * @brief The shell command that would install `command` on `os` ("linux",
 * "darwin", "windows"): the first one registered for that os ("unix" ones
 * count for every os but Windows, "any" ones for all) whose own program
 * (its first word, or the one after "sudo") is on PATH.
 * @param needs When none can run, set to the program the first one needed
 * ("npm", "brew"...), for the message; NULL if none is registered at all.
 * May itself be NULL.
 * @return The command (owned by the registry), or NULL.
 */
const char *lspInstallCommandFor(const char *command, const char *os, const char **needs);

#endif // BELUGA_LSP_INSTALL_H
