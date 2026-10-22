
#ifndef TERMINAL_H_
#define TERMINAL_H_

/* includes */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "utf8.h"

/**
 * @brief Where the editor's screen is. Standalone, that is the terminal
 * beluga runs in. In daemon mode (daemon.c) it is the client window being
 * served at the moment: drawing, key reads and the window size all go
 * through these three, never to os_term_*() directly.
 */
typedef struct
{
    void (*write)(const void *buf, size_t n);
    // os_stream_read() semantics: >0 bytes, 0 nothing in time, -1 never.
    long (*read_keys)(void *buf, size_t n, int timeout_ms);
    int (*size)(int *rows, int *cols);
} ScreenDriver;

// NULL: back to the real terminal.
void screenSetDriver(const ScreenDriver *driver);
void screenWrite(const void *buf, size_t n);
long screenReadKeys(void *buf, size_t n, int timeout_ms);

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

// Closes the debug log at exit (its stdio buffer is the last thing beluga
// would otherwise still hold when the memory report is written).
void appDebugClose(void);

#endif
