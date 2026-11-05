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
    os_term_write(MOUSE_OFF_SEQ, MOUSE_SEQ_LEN);
    os_term_write(EXTENDED_KEYS_OFF_SEQ, EXTENDED_KEYS_SEQ_LEN);
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
    os_term_write(MOUSE_OFF_SEQ, MOUSE_SEQ_LEN);
    os_term_write(EXTENDED_KEYS_OFF_SEQ, EXTENDED_KEYS_SEQ_LEN);
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

/**
 * @brief The name of a key without modifiers, as keyToString() gives it,
 * except that the space bar is "SPACE".
 */
static void plainKeyName(const unsigned char c, char *out, const size_t size)
{
    static const char ctrl_symbols[] = "\\]^_"; // 0x1C-0x1F

    switch (c)
    {
    case '\r':        snprintf(out, size, "ENTER");       return;
    case '\t':        snprintf(out, size, "TAB");         return;
    case ' ':         snprintf(out, size, "SPACE");       return;
    case ARROW_UP:    snprintf(out, size, "ARROW-UP");    return;
    case ARROW_DOWN:  snprintf(out, size, "ARROW-DOWN");  return;
    case ARROW_LEFT:  snprintf(out, size, "ARROW-LEFT");  return;
    case ARROW_RIGHT: snprintf(out, size, "ARROW-RIGHT"); return;
    case PAGE_UP:     snprintf(out, size, "PAGE-UP");     return;
    case PAGE_DOWN:   snprintf(out, size, "PAGE-DOWN");   return;
    case DEL_KEY:     snprintf(out, size, "DEL");         return;
    case BACKSPACE:   snprintf(out, size, "BACKSPACE");   return;
    case BEG_LINE:    snprintf(out, size, "HOME");        return;
    case END_LINE:    snprintf(out, size, "END");         return;
    case WHEEL_UP:    snprintf(out, size, "WHEEL-UP");    return;
    case WHEEL_DOWN:  snprintf(out, size, "WHEEL-DOWN");  return;
    case MOUSE_LEFT:  snprintf(out, size, "MOUSE-LEFT");  return;
    case MOUSE_OTHER: snprintf(out, size, "MOUSE-OTHER"); return;
    case '\x1b':      snprintf(out, size, "ESCAPE");      return;
    default:
        break;
    }
    if (c == 0)
        // Ctrl+Space (and, on most terminals, indistinguishably Ctrl+@)
        // sends a literal NUL byte — Space is 0x20, and Ctrl clears bits
        // 5-6, landing on 0. Named separately from the CTRL-<letter> range
        // just below (which starts at 1, deliberately excluding this):
        // 0 isn't "CTRL-`" in any convention a user would actually type.
        snprintf(out, size, "CTRL-SPACE");
    else if (c >= 1 && c <= 26)
        snprintf(out, size, "CTRL-%c", 'a' + c - 1);
    else if (c >= 0x1C && c <= 0x1F)
        snprintf(out, size, "CTRL-%c", ctrl_symbols[c - 0x1C]);
    else if (IS_FUNCTION_KEY(c) && c >= SHIFT_FUNCTION_KEY)
        snprintf(out, size, "SHIFT-F%d", c - SHIFT_FUNCTION_KEY + 1);
    else if (IS_FUNCTION_KEY(c))
        snprintf(out, size, "F%d", c - FUNCTION_KEY + 1);
    else if (isprint(c))
        snprintf(out, size, "%c", c);
    else
        snprintf(out, size, "KEY-%d", c);
}

char* keyToString(const Utf8Char* key)
{
    static char key_str[48];
    const unsigned char c = key->character[0];
    const int mods = key->character[1];
    int n;

    if (key->length > 1)
    {
        /* Genuine multi-byte UTF-8 codepoint: bytes are already
           stored raw in key->character, no re-encoding needed. */
        n = (unsigned char) key->length;
        memcpy(key_str, key->character, n);
        key_str[n] = '\0';
    }
    else if (IS_MOD_KEY(key))
    {
        n = snprintf(key_str, sizeof(key_str), "%s%s%s",
                     (mods & KEY_MOD_CTRL) ? "CTRL-" : "",
                     (mods & KEY_MOD_ALT) ? "ALT-" : "",
                     (mods & KEY_MOD_SHIFT) ? "SHIFT-" : "");
        plainKeyName(key->character[2], key_str + n, sizeof(key_str) - n);
    }
    else if (c == ' ')
        strcpy(key_str, " "); // map-key " ": the plain space bar
    else
        plainKeyName(c, key_str, sizeof(key_str));
    return key_str;
}

static int isLetter(const unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void keyMake(Utf8Char *key, int mods, unsigned char base)
{
    static const char ctrl_symbols[] = "\\]^_";
    const char *symbol;

    if (isLetter(base))
    {
        // Ctrl has no case: Ctrl+Shift+a is CTRL-SHIFT-a. Otherwise Shift
        // is the capital, as the terminal sends it (Alt+Shift+a is ESC A).
        if (mods & KEY_MOD_CTRL)
        {
            if (base <= 'Z')
                mods |= KEY_MOD_SHIFT;
            base = (unsigned char) (base | 0x20);
        }
        else if (mods & KEY_MOD_SHIFT)
        {
            base = (unsigned char) (base & ~0x20);
            mods &= ~KEY_MOD_SHIFT;
        }
    }
    else if (base > ' ' && base < 0x7F)
        mods &= ~KEY_MOD_SHIFT; // "!" is already Shift+1

    memset(key, 0, sizeof(*key));
    key->length = 1;
    key->character[0] = base;
    if (mods == 0)
        return;
    if (mods == KEY_MOD_CTRL)
    {
        // The keys Ctrl has a byte for, as every terminal sends them.
        if (isLetter(base))
            return (void) (key->character[0] = (unsigned char) (base - 'a' + 1));
        if (base == ' ' || base == '@')
            return (void) (key->character[0] = 0);
        if (base && (symbol = strchr(ctrl_symbols, base)) != NULL)
            return (void) (key->character[0] = (unsigned char) (0x1C + (symbol - ctrl_symbols)));
    }
    if (mods == KEY_MOD_SHIFT && base >= FUNCTION_KEY && base < FUNCTION_KEY + 12)
        return (void) (key->character[0] = (unsigned char) (base - FUNCTION_KEY + SHIFT_FUNCTION_KEY));
    key->character[0] = MOD_KEY;
    key->character[1] = (unsigned char) mods;
    key->character[2] = base;
}

int keySplit(const Utf8Char *key, unsigned char *base)
{
    const unsigned char c = key->character[0];

    *base = c;
    if (key->length != 1)
        return 0;
    if (IS_MOD_KEY(key))
    {
        *base = key->character[2];
        return key->character[1];
    }
    if (c == 0)
    {
        *base = ' ';
        return KEY_MOD_CTRL;
    }
    // TAB, ENTER (and Ctrl+j's newline) are keys of their own.
    if (c >= 1 && c <= 26 && c != '\t' && c != '\r' && c != '\n')
    {
        *base = (unsigned char) ('a' + c - 1);
        return KEY_MOD_CTRL;
    }
    if (c >= 0x1C && c <= 0x1F)
    {
        *base = (unsigned char) "\\]^_"[c - 0x1C];
        return KEY_MOD_CTRL;
    }
    if (c >= SHIFT_FUNCTION_KEY && c < SHIFT_FUNCTION_KEY + 12)
    {
        *base = (unsigned char) (c - SHIFT_FUNCTION_KEY + FUNCTION_KEY);
        return KEY_MOD_SHIFT;
    }
    return 0;
}

int keyFallback(const Utf8Char *key, Utf8Char *out)
{
    const int mods = key->character[1];
    const unsigned char base = key->character[2];

    if (!IS_MOD_KEY(key))
        return 0;
    if (mods == KEY_MOD_ALT && (base == 'f' || base == 'b'))
        keyMake(out, KEY_MOD_ALT, base == 'f' ? ARROW_RIGHT : ARROW_LEFT);
    else if (base >= 0x80 || base == '\x1b' || base == '\r' || base == '\t' || base == BACKSPACE)
        // Ctrl+Up as Up, Alt+Enter as Enter.
        keyMake(out, 0, base);
    else
        // Alt+a types "a", Ctrl+Alt+a is Ctrl+a, Ctrl+1 is "1".
        keyMake(out, mods & KEY_MOD_ALT ? mods & ~KEY_MOD_ALT : 0, base);
    return 1;
}

int keyEscapeSequence(const Utf8Char *key, char *out, const size_t size)
{
    static const int tilde_fkeys[12] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
    unsigned char base;
    int mods = keySplit(key, &base);
    const int xterm_mod = 1 + mods; // KEY_MOD_* bits are xterm's
    const char *alt = (mods & KEY_MOD_ALT) ? "\x1b" : "";
    char final = 0;
    int tilde = 0;
    Utf8Char plain;

    if (key->length > 1)
    {
        if ((size_t) key->length >= size)
            return 0;
        memcpy(out, key->character, (size_t) key->length);
        return key->length;
    }
    switch (base)
    {
    case ARROW_UP:    final = 'A'; break;
    case ARROW_DOWN:  final = 'B'; break;
    case ARROW_RIGHT: final = 'C'; break;
    case ARROW_LEFT:  final = 'D'; break;
    case BEG_LINE:    final = 'H'; break;
    case END_LINE:    final = 'F'; break;
    case DEL_KEY:     tilde = 3; break;
    case PAGE_UP:     tilde = 5; break;
    case PAGE_DOWN:   tilde = 6; break;
    default:
        if (base >= FUNCTION_KEY && base < FUNCTION_KEY + 12)
        {
            if (base < FUNCTION_KEY + 4)
                final = (char) ('P' + base - FUNCTION_KEY);
            else
                tilde = tilde_fkeys[base - FUNCTION_KEY];
        }
        break;
    }
    if (tilde)
        return snprintf(out, size, mods ? "\x1b[%d;%d~" : "\x1b[%d~", tilde, xterm_mod);
    if (final && mods)
        return snprintf(out, size, "\x1b[1;%d%c", xterm_mod, final);
    if (final)
        return snprintf(out, size, base >= FUNCTION_KEY ? "\x1bO%c" : "\x1b[%c", final);
    if (base >= 0x80)
        return 0; // UNKNOWN_KEY, mouse reports: nothing a program would read
    if (base == '\t' && mods == KEY_MOD_SHIFT)
        return snprintf(out, size, "\x1b[Z");
    // A character: what it is with Ctrl applied, after ESC for Alt. Ctrl
    // on a key without a control byte (Ctrl+1) or Shift on Enter: as
    // modifyOtherKeys reports it.
    keyMake(&plain, mods & ~KEY_MOD_ALT, base);
    if (IS_MOD_KEY(&plain))
        return snprintf(out, size, "\x1b[27;%d;%d~", xterm_mod, base);
    if ((size_t) plain.length + 2 > size)
        return 0;
    snprintf(out, size, "%s", alt);
    memcpy(out + strlen(alt), plain.character, (size_t) plain.length);
    out[strlen(alt) + (size_t) plain.length] = '\0';
    return (int) strlen(alt) + plain.length;
}

/**
 * @brief If `name` starts with a modifier ("CTRL-", "M-", ...) followed
 * by more, its KEY_MOD_* bit and the length of the prefix, else 0.
 */
static int modifierPrefix(const char *name, int *len)
{
    static const struct { const char *prefix; int mod; } prefixes[] = {
        {"CTRL-", KEY_MOD_CTRL}, {"C-", KEY_MOD_CTRL},
        {"ALT-", KEY_MOD_ALT}, {"OPTION-", KEY_MOD_ALT}, {"OPT-", KEY_MOD_ALT},
        {"META-", KEY_MOD_ALT}, {"M-", KEY_MOD_ALT},
        {"SHIFT-", KEY_MOD_SHIFT}, {"S-", KEY_MOD_SHIFT},
    };
    size_t i;
    size_t n;

    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        n = strlen(prefixes[i].prefix);
        if (strncmp(name, prefixes[i].prefix, n) == 0 && name[n] != '\0')
        {
            *len = (int) n;
            return prefixes[i].mod;
        }
    }
    return 0;
}

/**
 * @brief The key a name without modifiers stands for, the other way round
 * from plainKeyName(). @return 0 if it isn't one character or known name.
 */
static int plainKeyFromName(const char *name, unsigned char *base)
{
    static const struct { const char *name; unsigned char key; } names[] = {
        {"ENTER", '\r'}, {"TAB", '\t'}, {"SPACE", ' '}, {"ESCAPE", '\x1b'},
        {"BACKSPACE", BACKSPACE}, {"DEL", DEL_KEY}, {"HOME", BEG_LINE}, {"END", END_LINE},
        {"PAGE-UP", PAGE_UP}, {"PAGE-DOWN", PAGE_DOWN},
        {"ARROW-UP", ARROW_UP}, {"ARROW-DOWN", ARROW_DOWN},
        {"ARROW-LEFT", ARROW_LEFT}, {"ARROW-RIGHT", ARROW_RIGHT},
    };
    size_t i;
    int n;

    if (name[0] != '\0' && name[1] == '\0' && (unsigned char) name[0] < 0x80)
    {
        *base = (unsigned char) name[0];
        return 1;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcmp(name, names[i].name) == 0)
        {
            *base = names[i].key;
            return 1;
        }
    }
    if (name[0] == 'F' && sscanf(name + 1, "%d", &n) == 1 && n >= 1 && n <= 12)
    {
        *base = (unsigned char) (FUNCTION_KEY + n - 1);
        return 1;
    }
    return 0;
}

void keyCanonicalName(const char *name, char *out, const size_t size)
{
    int mods = 0;
    int mod;
    int len;
    const char *rest = name;
    unsigned char base;
    Utf8Char key;

    while ((mod = modifierPrefix(rest, &len)) != 0)
    {
        mods |= mod;
        rest += len;
    }
    // "CTRL-A" is what people write for Ctrl+a, not Ctrl+Shift+a.
    if (mods == KEY_MOD_CTRL && rest[0] >= 'A' && rest[0] <= 'Z' && rest[1] == '\0')
    {
        snprintf(out, size, "CTRL-%c", rest[0] | 0x20);
        return;
    }
    if (mods == 0 || !plainKeyFromName(rest, &base))
    {
        snprintf(out, size, "%s", name);
        return;
    }
    keyMake(&key, mods, base);
    snprintf(out, size, "%s", keyToString(&key));
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
    // Not mouse reports: replayed, they'd act wherever the pointer is then.
    if (c->length != 1 || !IS_MOUSE_KEY(c->character[0]))
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
