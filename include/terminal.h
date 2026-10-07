
#ifndef TERMINAL_H_
#define TERMINAL_H_

/* includes */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "utf8.h"

/**
 * \fn void die(const char *s)
 * \brief Exit program and return s error message.
 * \param s Error string
 * @note Marked noreturn: it always exit()s. Without this a static analyzer
 * (and a human skimming the caller) has to assume execution can continue
 * past a `foo == NULL ? die(...) : ...` guard, which reads as a real null
 * dereference a few lines later even though it can never actually happen.
 * */

void die(const char *s) __attribute__((noreturn));

void disableRawMode();
// Restore the terminal for the shell: reset attributes, clear, show cursor,
// leave raw mode. Called once on the way out of editorRunLoop().
void terminalRestore(void);

void enableRawMode();

Utf8Char* editorReadKey();

int getWindowSize(int *rows, int *cols);

char *keyToString(const Utf8Char* key);

void appDebug(const char *fmt, ...);

// Path of the debug log appDebug() writes to (opened by this call if not
// yet), or NULL when built without APP_DEBUG. Lets another subsystem —
// currently just the spawned language server — send its own stderr there
// too instead of onto the terminal beluga is drawing on.
const char *appDebugLogPath(void);

#endif
