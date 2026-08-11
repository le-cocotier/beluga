/**
 * @file buffer_switcher.c
 * @brief CTRL-x b buffer switcher — an Emacs-style popup listing every
 * open buffer, navigated with the arrow keys, ENTER to switch.
 * @details Deliberately never snapshots buffers into an array: E.buffers
 * is a circular doubly-linked list with no fixed capacity, so both
 * navigation and rendering walk it live via ->next/->prev every time.
 */

#include "../include/buffer_switcher.h"
#include "../include/buffer.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/define.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

// ─── ANSI helpers (same small private set lsp_ui.c keeps for itself) ──────────

static void ab_move(struct abuf *ab, int row, int col) {
    char esc[32];
    int n = snprintf(esc, sizeof(esc), "\x1b[%d;%dH", row, col);
    abAppend(ab, esc, n);
}

static void ab_sgr(struct abuf *ab, const char *codes) {
    char esc[32];
    int n = snprintf(esc, sizeof(esc), "\x1b[%sm", codes);
    abAppend(ab, esc, n);
}

#define AB_RESET(ab) ab_sgr(ab, "0")

/**
 * @brief Formats one buffer's display line: "<id>: <filename> [*]".
 */
static void formatBufferLine(struct buffer_t *b, char *out, size_t out_cap) {
    char *filename = (char *) utf8ToString(b->filename);
    snprintf(out, out_cap, "%d: %s%s", b->buffer_id,
             (filename && filename[0]) ? filename : "[No Name]",
             b->dirty ? " *" : "");
    free(filename);
}

void bufferSwitcherOpen(void) {
    if (E.buffers == NULL)
        return;

    EditorPane *active = splitScreenGetActivePane();
    E.buffer_switcher.current_buffer_id = active->buffer_id;
    E.buffer_switcher.visible = 1;
}

int bufferSwitcherHandleKey(Utf8Char *key) {
    if (!E.buffer_switcher.visible)
        return 0;

    struct buffer_t *current = bufferFindById(E.buffer_switcher.current_buffer_id);
    if (!current) {
        // The highlighted buffer vanished from under us (e.g. closed by
        // something else while the popup was open) — just bail out rather
        // than navigate from nowhere.
        E.buffer_switcher.visible = 0;
        return 1;
    }

    unsigned char c = key->character[0];
    int is_single = (key->length == 1);

    if (is_single && c == '\x1b') { // ESC — cancel
        E.buffer_switcher.visible = 0;
        return 1;
    }
    if (is_single && (c == ARROW_DOWN || c == CTRL_KEY('n'))) {
        E.buffer_switcher.current_buffer_id = current->next->buffer_id;
        return 1;
    }
    if (is_single && (c == ARROW_UP || c == CTRL_KEY('p'))) {
        E.buffer_switcher.current_buffer_id = current->prev->buffer_id;
        return 1;
    }
    if (is_single && c == '\r') { // ENTER — switch to the highlighted buffer
        bufferSwitch(current->buffer_id);
        E.buffer_switcher.visible = 0;
        return 1;
    }

    // This is a modal picker, not a live-filtering popup like completion —
    // stray keys shouldn't leak through and get typed into whatever buffer
    // was active before it opened, so just swallow them.
    return 1;
}

void bufferSwitcherDraw(struct abuf *ab) {
    if (!E.buffer_switcher.visible || E.buffers == NULL)
        return;

    // First pass: measure. Walk the whole (unbounded) list once to find
    // how many buffers there are and how wide the box needs to be.
    int count = 0;
    int content_w = 0;
    int first_buffer_id = E.buffers->buffer_id;
    struct buffer_t *b = E.buffers;
    do {
        char line[160];
        formatBufferLine(b, line, sizeof(line));
        int len = (int) strlen(line);
        if (len > content_w) content_w = len;
        count++;
        b = b->next;
    } while (b->buffer_id != first_buffer_id);

    if (content_w > E.screencols - 4) content_w = E.screencols - 4;
    if (content_w < 10) content_w = 10;
    int box_w = content_w + 4; // "| " + text + " |"
    int box_h = count + 2; // rows + top/bottom border

    int origin_x = (E.screencols - box_w) / 2;
    int origin_y = (E.screenrows - box_h) / 2;
    if (origin_x < 1) origin_x = 1;
    if (origin_y < 1) origin_y = 1;

    // ── top border ──────────────────────────────────────────────────────────
    ab_move(ab, origin_y, origin_x);
    ab_sgr(ab, "0;37;40");
    abAppend(ab, "┌", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┐", 3);

    // ── rows: second pass, walking the list again to draw ──────────────────
    int row = 0;
    b = E.buffers;
    do {
        int is_sel = (b->buffer_id == E.buffer_switcher.current_buffer_id);

        ab_move(ab, origin_y + 1 + row, origin_x);
        ab_sgr(ab, is_sel ? "7" : "0;37;40");

        abAppend(ab, "| ", 2);

        char line[160];
        formatBufferLine(b, line, sizeof(line));
        char padded[160];
        snprintf(padded, sizeof(padded), "%-*s", content_w, line);
        padded[content_w] = '\0'; // truncate if the line itself overruns content_w
        abAppend(ab, padded, content_w);

        abAppend(ab, " |", 2);
        AB_RESET(ab);

        row++;
        b = b->next;
    } while (b->buffer_id != first_buffer_id);

    // ── bottom border ───────────────────────────────────────────────────────
    ab_move(ab, origin_y + 1 + count, origin_x);
    ab_sgr(ab, "0;37;40");
    abAppend(ab, "└", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┘", 3);
    AB_RESET(ab);
}
