#include "../include/terminal.h"
#include "../include/macro.h"

#include <ctype.h>

#include "../include/data.h"
#include "../include/define.h"
#include "../include/os.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../include/utf8.h"
#include "../include/utils.h"

/***********************************************************************************************************************
 *
 *                                                 extern definitions
 *
 **********************************************************************************************************************/

extern EditorConfig E;

/***********************************************************************************************************************
 *
 *                                                 functions definitions
 *
 **********************************************************************************************************************/


static long termReadKeys(void *buf, const size_t n, const int timeout_ms)
{
    return os_stream_read(os_term_input(), buf, n, timeout_ms);
}

static const ScreenDriver s_terminal_driver = {os_term_write, termReadKeys, os_term_size};
static const ScreenDriver *s_screen = &s_terminal_driver;

void screenSetDriver(const ScreenDriver *driver)
{
    s_screen = driver ? driver : &s_terminal_driver;
}

void screenWrite(const void *buf, const size_t n)
{
    s_screen->write(buf, n);
}

long screenReadKeys(void *buf, const size_t n, const int timeout_ms)
{
    return s_screen->read_keys(buf, n, timeout_ms);
}

void die(const char* s)
{
    os_term_write("\x1b[2J", 4);
    os_term_write(CURSOR_TOP_LEFT, 3);
    os_term_leave_raw();
    lisp_shutdown(E.ctx);
    perror(s);
    exit(1);
}

void disableRawMode()
{
    os_term_leave_raw();
}

/**
 * @brief Hand the terminal back to the shell in a usable state.
 * @details Order matters: reset SGR *before* clearing, or the theme's
 * background color is what the clear paints the screen with and it stays
 * as the shell's background afterward. Also re-shows the cursor, which
 * editorRefreshScreen() hides on every frame — quitting mid-frame used to
 * leave an invisible cursor behind. Safe to call more than once.
 */
void terminalRestore(void)
{
    os_term_write("\x1b[0m", 4);   // reset colors/attributes
    os_term_write("\x1b[2J", 4);   // clear screen
    os_term_write(CURSOR_TOP_LEFT, 3);
    os_term_write(SHOW_CURSOR, 6);
    disableRawMode();
}

void enableRawMode()
{
    if (os_term_enter_raw() == -1)
    {
        // Not a terminal, or (Windows) one too old for VT sequences.
        fprintf(stderr, "beluga: this terminal can't be put in raw mode%s\n",
                strcmp(os_name(), "windows") == 0 ? " (Windows 10 1809 or later needed)" : "");
        exit(1);
    }
}

char* keyToString(const Utf8Char* key)
{
    static char key_str[32];
    const unsigned char c = key->character[0];
    int n;

    if (c == '\r')
    {
        strcpy(key_str, "ENTER");
    }
    else if (c == 0x09)
    {
        strcpy(key_str, "TAB");
    }
    else if (c == 0x00)
    {
        // Ctrl+Space (and, on most terminals, indistinguishably Ctrl+@)
        // sends a literal NUL byte — Space is 0x20, and Ctrl clears bits
        // 5-6, landing on 0. Named separately from the CTRL-<letter> range
        // just below (which starts at 1, deliberately excluding this):
        // 0 isn't "CTRL-`" in any convention a user would actually type.
        strcpy(key_str, "CTRL-SPACE");
    }
    else if (c >= 1 && c <= 26)
    {
        snprintf(key_str, sizeof(key_str), "CTRL-%c", 'a' + c - 1);
    }
    else
    {
        switch (c)
        {
        case ARROW_UP:    strcpy(key_str, "ARROW-UP");    break;
        case ARROW_DOWN:  strcpy(key_str, "ARROW-DOWN");  break;
        case ARROW_LEFT:  strcpy(key_str, "ARROW-LEFT");  break;
        case ARROW_RIGHT: strcpy(key_str, "ARROW-RIGHT"); break;
        case ALT_ARROW_LEFT:  strcpy(key_str, "ALT-ARROW-LEFT");  break;
        case ALT_ARROW_RIGHT: strcpy(key_str, "ALT-ARROW-RIGHT"); break;
        case CTRL_DEL_KEY:    strcpy(key_str, "CTRL-DEL");        break;
        case PAGE_UP:     strcpy(key_str, "PAGE-UP");     break;
        case PAGE_DOWN:   strcpy(key_str, "PAGE-DOWN");   break;
        case DEL_KEY:     strcpy(key_str, "DEL");         break;
        case BACKSPACE:   strcpy(key_str, "BACKSPACE");   break;
        case BEG_LINE:    strcpy(key_str, "HOME");        break;
        case END_LINE:    strcpy(key_str, "END");         break;
        case ALT_X:       strcpy(key_str, "ALT-x");       break;
        case '\x1b':      strcpy(key_str, "ESCAPE");      break;
        default:
            if (key->length > 1)
            {
                /* Genuine multi-byte UTF-8 codepoint: bytes are already
                   stored raw in key->character, no re-encoding needed. */
                n = (unsigned char) key->length;
                memcpy(key_str, key->character, n);
                key_str[n] = '\0';
            }
            else if (isprint(c))
            {
                snprintf(key_str, sizeof(key_str), "%c", c);
            }
            else
            {
                snprintf(key_str, sizeof(key_str), "KEY-%d", c);
            }
        }
    }
    return key_str;
}

/**
 * @note Memory allocated for the return char
 */
Utf8Char * editorReadKey()
{
    Utf8Char* c;

    // A macro being replayed comes first, then the terminal.
    c = macroNextReplayedKey();
    if (c)
        return c;
    c = readUtf8Char();
    macroRecordKey(c);
    return c;
}

int getWindowSize(int* rows, int* cols)
{
    return s_screen->size(rows, cols);
}

#ifdef APP_DEBUG
/**
 * @brief ~/.beluga/debug.log, where appDebug() writes.
 */
static const char *debugLogPath(void)
{
    static char path[PATH_MAX];
    if (!path[0])
        snprintf(path, sizeof(path), "%s/debug.log", utilBelugaDir());
    return path;
}

/**
 * @brief Opens (once) and returns the debug log file at ~/.beluga/debug.log.
 * @details Never stderr: this process's stderr is the same raw terminal
 * it's drawing its UI onto, so anything written there directly corrupts
 * the screen instead of scrolling by underneath it — which is exactly
 * what made a debug build "unusable" before this existed. Truncated
 * ("w", not "a") on open so each run starts its own clean log instead of
 * appending to however many past sessions came before it. Line-buffered
 * and explicitly flushed on every appDebug() call (see below) so a crash
 * or a hang doesn't lose whatever was written right before it — the
 * exact moment a debug log is actually needed.
 */
static FILE *s_debug_log = NULL;
static int s_debug_log_tried = 0;

static FILE *debugLogFile(void)
{
    if (!s_debug_log_tried)
    {
        s_debug_log_tried = 1;
        s_debug_log = fopen(debugLogPath(), "w");
        if (s_debug_log)
            setvbuf(s_debug_log, NULL, _IOLBF, 0);
    }
    return s_debug_log;
}
#endif

void appDebugClose(void)
{
#ifdef APP_DEBUG
    // Stays closed: appDebug() after this writes nothing.
    if (s_debug_log)
        fclose(s_debug_log);
    s_debug_log = NULL;
    s_debug_log_tried = 1;
#endif
}

const char *appDebugLogPath(void)
{
#ifdef APP_DEBUG
    return debugLogFile() ? debugLogPath() : NULL;
#else
    return NULL;
#endif
}

void appDebug(const char* fmt, ...)
{
#ifdef APP_DEBUG
    FILE *f = debugLogFile();
    long long ms;
    va_list ap;
    if (!f)
        return;

    ms = os_monotonic_ms();
    fprintf(f, "[%6lld.%03lld] ", ms / 1000, ms % 1000);

    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
#endif
    (void) fmt;
}
