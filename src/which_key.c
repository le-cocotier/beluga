/**
 * @file which_key.c
 * @brief Emacs which-key style discoverability panel — shows what you can
 * press next while a key prefix is pending, and which commands match what
 * you've typed at the M-x prompt.
 */

#include "../include/which_key.h"
#include "../include/galloc.h"

#include "../include/append_buffer.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/input.h"
#include "../include/popup_ui.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"

#include "../include/os.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern EditorConfig E;

// Same tiny abuf helpers the other overlay modules (lsp_ui, list_popup,
// buffer_switcher) each keep locally.
static void ab_move(struct abuf *ab, const int row, int col) {
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    abAppend(ab, buf, n);
}

static void ab_sgr(struct abuf *ab, const char *codes) {
    // 48, not the 32 every other popup module's copy of this helper uses:
    // this file's ab_sgr is fed PopupStyle's combined bold+bg+fg codes
    // (up to ~35 chars), not the short bare color codes ("33", "1;31")
    // everything else here passes. 32 silently truncated them — the
    // clamp below stopped that from being a buffer over-read, but the
    // codes still came out cut off mid-escape-sequence and broke the
    // panel's colors until the actual buffer was sized to fit.
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\x1b[%sm", codes);
    if (n >= (int) sizeof(buf)) n = (int) sizeof(buf) - 1; // defensive: never over-read on truncation
    abAppend(ab, buf, n);
}

#define AB_RESET(ab) ab_sgr(ab, "0")

#define WK_MAX_ROWS       8    // panel never eats more than this many rows
#define WK_ENTRY_MAX     64
#define WK_MIN_COL_WIDTH 18

// How long a prefix must stay pending before the panel appears. Long enough
// that a fluent "CTRL-x CTRL-s" doesn't flash the panel on every save, short
// enough that hesitating gets you help. Emacs' which-key defaults to 1s;
// this is snappier because the panel is small.
#define WK_DELAY_MS 400

// ─── caches ────────────────────────────────────────────────────────────────
//
// Both of these exist to keep per-frame work down to plain strcmp/strncmp.
// Resolving a Lisp value back to the symbol it's bound to means walking the
// global environment into an alist, which allocates — unacceptable at the
// ~20 redraws/second the panel is drawn at.

static char **g_bind_names = NULL;  // parallel to E.key_binds
static int g_bind_names_count = 0;

static char **g_commands = NULL;    // M-x candidates
static int g_command_count = 0;

static long monotonic_ms(void) {
    return (long) os_monotonic_ms();
}

/**
 * @brief Reverse-lookup: the symbol name a value is bound to in the global
 * environment, or NULL.
 * @details map-key receives its command already evaluated — `editor-quit`
 * arrives as the function object itself, not the symbol — so the printable
 * name has to be recovered by searching the environment for a binding with
 * this exact value. lisp_equal compares C function pointers for LISP_FUNC
 * and object identity for lambdas, which is exactly the notion of "same
 * command" wanted here.
 */
static const char *symbolNameForValue(const Lisp value, const Lisp alist) {
    const LispType want = lisp_type(value);
    Lisp it;
    Lisp pair;
    Lisp key;
    Lisp val;
    int same;
    if (want != LISP_FUNC && want != LISP_LAMBDA)
        return NULL; // only commands are worth naming

    for (it = alist; lisp_is_pair(it); it = lisp_cdr(it)) {
        pair = lisp_car(it);
        if (!lisp_is_pair(pair))
            continue;
        key = lisp_car(pair);
        if (lisp_type(key) != LISP_SYMBOL)
            continue;

        val = lisp_cdr(pair);
        if (lisp_type(val) != want)
            continue;

        // Deliberately NOT lisp_equal(): its LISP_FUNC branch is
        // `lisp_func(a) == lisp_func(b) && a.type == b.type`, and both
        // calls are evaluated before that type guard is reached — so
        // comparing a function against the first non-function binding in
        // the environment trips lisp_func()'s assert (and reads the wrong
        // union member outright once asserts are compiled out). Having
        // already matched the types above, compare the payloads directly.
        same = (want == LISP_FUNC) ? (lisp_func(val) == lisp_func(value))
                                       : lisp_eq(val, value);
        if (same)
            return lisp_symbol_string(key);
    }
    return NULL;
}

static void freeStrings(char ***arr, int *count) {
    int i;
    if (*arr) {
        for (i = 0; i < *count; i++)
            gfree((*arr)[i]);
        gfree(*arr);
    }
    *arr = NULL;
    *count = 0;
}

void whichKeyInvalidate(void) { freeStrings(&g_bind_names, &g_bind_names_count); }

/**
 * @brief Fill g_bind_names with a printable command name per keybinding.
 * @details Two shapes show up in E.key_binds[].command: a quoted list like
 * '(editor-set-prefix "user"), whose head symbol is the name (plus its
 * string argument, which is what distinguishes one prefix-setter from
 * another), and a bare evaluated function, which needs the reverse lookup.
 */
static void buildBindNames(void) {
    Lisp alist;
    int i;
    char name[WK_ENTRY_MAX];
    Lisp cmd;
    Lisp head;
    Lisp rest;
    Lisp arg;
    const char *resolved;
    if (g_bind_names && g_bind_names_count == E.number_of_keybinds)
        return; // still current

    freeStrings(&g_bind_names, &g_bind_names_count);
    if (E.number_of_keybinds <= 0)
        return;

    g_bind_names = gcalloc((size_t) E.number_of_keybinds, sizeof(char *));
    if (!g_bind_names)
        return;
    g_bind_names_count = E.number_of_keybinds;

    alist = lisp_table_to_alist(lisp_car(lisp_env(E.ctx)), E.ctx);

    for (i = 0; i < E.number_of_keybinds; i++) {
        cmd = E.key_binds[i].command;

        name[0] = '?';
        name[1] = '\0';

        if (lisp_is_pair(cmd)) {
            head = lisp_car(cmd);
            if (lisp_type(head) == LISP_SYMBOL) {
                rest = lisp_cdr(cmd);
                arg = lisp_is_pair(rest) ? lisp_car(rest) : lisp_null();
                if (lisp_type(arg) == LISP_STRING)
                    snprintf(name, sizeof(name), "%s %s",
                             lisp_symbol_string(head), lisp_string(arg));
                else
                    snprintf(name, sizeof(name), "%s", lisp_symbol_string(head));
            }
        } else {
            resolved = symbolNameForValue(cmd, alist);
            if (resolved)
                snprintf(name, sizeof(name), "%s", resolved);
        }
        g_bind_names[i] = utilStrdup(name);
    }
}

void whichKeyBuildCommandList(void) {
    Lisp alist;
    int cap;
    Lisp it;
    Lisp pair;
    Lisp key;
    Lisp val;
    char **grown;
    int i;
    char *k;
    int j;
    whichKeyFreeCommandList();

    alist = lisp_table_to_alist(lisp_car(lisp_env(E.ctx)), E.ctx);

    cap = 64;
    g_commands = gcalloc((size_t) cap, sizeof(char *));
    if (!g_commands)
        return;

    for (it = alist; lisp_is_pair(it); it = lisp_cdr(it)) {
        pair = lisp_car(it);
        if (!lisp_is_pair(pair))
            continue;
        key = lisp_car(pair);
        val = lisp_cdr(pair);
        if (lisp_type(key) != LISP_SYMBOL)
            continue;
        if (lisp_type(val) != LISP_FUNC && lisp_type(val) != LISP_LAMBDA)
            continue;

        if (g_command_count >= cap) {
            cap *= 2;
            grown = grealloc(g_commands, (size_t) cap * sizeof(char *));
            if (!grown)
                break;
            g_commands = grown;
        }
        g_commands[g_command_count++] = utilStrdup(lisp_symbol_string(key));
    }

    // Alphabetical: the env walk yields hash-table order, which looks random.
    for (i = 1; i < g_command_count; i++) {
        k = g_commands[i];
        j = i - 1;
        while (j >= 0 && strcmp(g_commands[j], k) > 0) {
            g_commands[j + 1] = g_commands[j];
            j--;
        }
        g_commands[j + 1] = k;
    }
}

void whichKeyFreeCommandList(void) { freeStrings(&g_commands, &g_command_count); }

/**
 * @brief Width of a UTF-8 string in terminal columns.
 * @details Counting bytes instead would inflate every entry containing the
 * "→" separator by 2 (3 bytes, 1 column), which both overestimates the
 * column width — dropping a 2-column layout to 1 — and misaligns the
 * "%-*s" padding that assumes bytes == columns.
 * @note Combining marks and double-width CJK are not accounted for; entries
 * here are key names and command symbols, which are ASCII plus the arrow.
 */
static int displayWidth(const char *s) {
    int w = 0;
    const unsigned char *p;
    for (p = (const unsigned char *) s; *p; p++)
        if ((*p & 0xC0) != 0x80) // skip UTF-8 continuation bytes
            w++;
    return w;
}

/**
 * @brief Append `s` followed by enough spaces to fill `width` columns.
 * @return columns actually written.
 */
static int abAppendPadded(struct abuf *ab, const char *s, const int width, const int budget) {
    int w = displayWidth(s);
    int bytes;
    int cols;
    const unsigned char *p;
    int pad;
    int i;
    if (w > budget) w = budget; // caller guarantees budget >= 0
    bytes = 0; cols = 0;
    for (p = (const unsigned char *) s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            if (cols >= w) break;
            cols++;
        }
        bytes++;
    }
    abAppend(ab, s, bytes);
    pad = width - cols;
    if (pad > budget - cols) pad = budget - cols;
    for (i = 0; i < pad; i++) abAppend(ab, " ", 1);
    return cols + (pad > 0 ? pad : 0);
}

/**
 * @brief Draw `count` "label" strings as a multi-column grid filling the
 * bottom of the pane area, plus a title line.
 */
static void drawPanel(struct abuf *ab, const char *title,
                      char **entries, const int count) {
    int longest;
    int i;
    int len;
    int col_w;
    int cols;
    int rows;
    int truncated;
    PopupStyle style;
    int top;
    int shown;
    int c;
    char header[192];
    int r;
    int used;
    int idx;
    int budget;
    if (count <= 0)
        return;

    longest = 0;
    for (i = 0; i < count; i++) {
        len = displayWidth(entries[i]);
        if (len > longest)
            longest = len;
    }
    col_w = longest + 2;
    if (col_w < WK_MIN_COL_WIDTH)
        col_w = WK_MIN_COL_WIDTH;
    if (col_w > E.screencols)
        col_w = E.screencols;

    cols = E.screencols / col_w;
    if (cols < 1)
        cols = 1;

    rows = (count + cols - 1) / cols;
    truncated = 0;
    if (rows > WK_MAX_ROWS) {
        rows = WK_MAX_ROWS;
        truncated = 1;
    }

    popupStyleInit(&style);

    // Sit directly above the status bar, which is drawn at E.screenrows + 1.
    // -1 for the accent rule separating the panel from the buffer above it.
    top = E.screenrows - rows - 1;
    if (top < 1)
        top = 1;

    shown = rows * cols;
    if (shown > count)
        shown = count;

    // A thin accent-colored rule, so the panel reads as a distinct surface
    // dropped over the editor rather than text that happens to appear at
    // the bottom of it.
    ab_move(ab, top, 1);
    ab_sgr(ab, style.chrome);
    for (c = 0; c < E.screencols; c++) abAppend(ab, "─", 3);
    AB_RESET(ab);

    ab_move(ab, top + 1, 1);
    ab_sgr(ab, style.title);
    if (truncated && shown < count)
        snprintf(header, sizeof(header), " %s  (+%d more)", title, count - shown);
    else
        snprintf(header, sizeof(header), " %s", title);
    abAppendPadded(ab, header, E.screencols, E.screencols);
    AB_RESET(ab);

    for (r = 0; r < rows; r++) {
        ab_move(ab, top + 2 + r, 1);
        ab_sgr(ab, style.row);
        used = 0;
        for (c = 0; c < cols; c++) {
            // Column-major so the list reads top-to-bottom, like Emacs'.
            idx = c * rows + r;
            if (idx >= count)
                break;
            budget = E.screencols - used;
            if (budget <= 0)
                break;
            used += abAppendPadded(ab, entries[idx], col_w, budget);
        }
        for (; used < E.screencols; used++)
            abAppend(ab, " ", 1);
        AB_RESET(ab);
    }
}

/**
 * @brief M-x help: the commands whose names start with what's been typed.
 */
static void drawCommandHelp(struct abuf *ab) {
    char *typed;
    size_t typed_len;
    char **matches;
    int n;
    int i;
    char title[128];
    if (!g_commands || g_command_count == 0)
        return;

    typed = E.prompt_input ? (char *) utf8ToString(E.prompt_input) : NULL;
    typed_len = typed ? strlen(typed) : 0;

    matches = gcalloc((size_t) g_command_count, sizeof(char *));
    if (!matches) {
        gfree(typed);
        return;
    }
    n = 0;
    for (i = 0; i < g_command_count; i++) {
        if (typed_len == 0 || strncmp(g_commands[i], typed, typed_len) == 0)
            matches[n++] = g_commands[i];
    }

    snprintf(title, sizeof(title), " M-x — %d command%s%s%s", n,
             n == 1 ? "" : "s",
             typed_len ? " matching " : "", typed_len ? typed : "");

    if (n > 0)
        drawPanel(ab, title, matches, n);

    gfree(matches);
    gfree(typed);
}

/**
 * @brief Pending-prefix help: every key bound under the active prefix.
 */
static void drawPrefixHelp(struct abuf *ab) {
    char **entries;
    char *storage;
    int n;
    int i;
    char *slot;
    char title[128];
    const struct buffer_t *buf;
    int scope;
    int shadowed;
    int j;
    buildBindNames();
    if (!g_bind_names)
        return;

    entries = gcalloc((size_t) E.number_of_keybinds, sizeof(char *));
    storage = gcalloc((size_t) E.number_of_keybinds, WK_ENTRY_MAX);
    if (!entries || !storage) {
        gfree(entries);
        gfree(storage);
        return;
    }

    buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
    n = 0;
    for (i = 0; i < E.number_of_keybinds; i++) {
        if (E.key_binds[i].prefix_id != E.prefix_state)
            continue;
        // Only what the key would actually run in this buffer: skip other
        // languages' bindings, and a global one a language binding shadows.
        scope = keyBindScope(&E.key_binds[i], buf);
        if (scope == 0)
            continue;
        shadowed = 0;
        for (j = 0; scope == 1 && j < E.number_of_keybinds && !shadowed; j++)
            shadowed = j != i && E.key_binds[j].prefix_id == E.prefix_state &&
                       strcmp(E.key_binds[j].key_sequence, E.key_binds[i].key_sequence) == 0 &&
                       keyBindScope(&E.key_binds[j], buf) == 2;
        if (shadowed)
            continue;
        slot = storage + (size_t) n * WK_ENTRY_MAX;
        snprintf(slot, WK_ENTRY_MAX, "%s → %s", E.key_binds[i].key_sequence,
                 g_bind_names[i] ? g_bind_names[i] : "?");
        entries[n++] = slot;
    }

    if (n > 0) {
        snprintf(title, sizeof(title), " %s — %d key%s (CTRL-g to cancel)",
                 E.prefix[E.prefix_state].prefix_name, n, n == 1 ? "" : "s");
        drawPanel(ab, title, entries, n);
    }

    gfree(entries);
    gfree(storage);
}

void whichKeyDraw(struct abuf *ab) {
    EditorPane *active;
    struct buffer_t *buf;
    static int last_prefix = 0;
    static long pending_since = 0;
    long now;
    if (!E.constantes.WHICH_KEY)
        return;

    // The M-x prompt is explicitly asking "which command?", so its
    // candidate list shows straight away — no settle delay.
    if (E.prompt_is_command) {
        drawCommandHelp(ab);
        return;
    }

    if (E.prefix_state == 0)
        return;

    // A terminal pane parks prefix_state on "term" for as long as it's
    // focused (nothing but ESCAPE is bound there, so the usual auto-reset
    // never fires). That's ambient state rather than a pending chord —
    // showing a permanent panel over the shell would just eat rows.
    active = splitScreenGetActivePane();
    buf = active ? bufferFindById(active->buffer_id) : NULL;
    if (buf && buf->type == TERMINAL_BUFF)
        return;
    // A sticky prefix (explorer, search, dired, ...) is the same kind of
    // ambient state: it's the mode of the buffer you're in, not a chord
    // waiting for its second key, so it never "settles" into needing help.
    if (E.prefix[E.prefix_state].sticky)
        return;

    // Only once the prefix has been pending a moment, so fluent chords
    // don't strobe the panel on their way through.
    now = monotonic_ms();
    if (E.prefix_state != last_prefix) {
        last_prefix = E.prefix_state;
        pending_since = now;
    }
    if (now - pending_since < WK_DELAY_MS)
        return;

    drawPrefixHelp(ab);
}
