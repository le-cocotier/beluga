
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

// Modified keys (MOD_KEY in define.h).
// The key for `base` (a character or a key code) with KEY_MOD_* `mods`,
// in its one canonical form: Ctrl+a is the byte 0x01, Shift+a is "A",
// Alt+Shift+a is ALT-"A", Shift+F5 is SHIFT_FUNCTION_KEY + 4.
void keyMake(Utf8Char *key, int mods, unsigned char base);
// The other way round: `key`'s modifiers and the key without them (0x01
// gives KEY_MOD_CTRL and 'a'). A multi-byte character has no modifiers.
int keySplit(const Utf8Char *key, unsigned char *base);
// What a modified key used to be before keys carried every modifier,
// tried when it isn't bound: Ctrl+Up acts as Up, Alt+a types "a" (an
// AZERTY "{" with Option as Meta arrives as Alt+...), Alt+f and Alt+b as
// Alt+Right / Alt+Left (what Terminal.app sends for Option+arrows).
// @return 0 if `key` has no such fallback.
int keyFallback(const Utf8Char *key, Utf8Char *out);
// The bytes an xterm sends for `key`, for a program in a terminal pane.
// @return their length, 0 if it has none.
int keyEscapeSequence(const Utf8Char *key, char *out, size_t size);
// `name` as keyToString() would spell it: modifiers in CTRL-ALT-SHIFT
// order, with OPTION-/META-/OPT- and Emacs's C-/M-/S- accepted
// ("M-x" -> "ALT-x", "SHIFT-ALT-a" -> "ALT-A", "C-M-a" -> "CTRL-ALT-a").
// A name without modifiers is copied as is.
void keyCanonicalName(const char *name, char *out, size_t size);

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
