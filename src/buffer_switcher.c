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
#include "../include/popup_ui.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

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

    if (content_w > E.screencols - 6) content_w = E.screencols - 6;
    if (content_w < 10) content_w = 10;
    int box_w = content_w + 4; // border(2) + marker/margin(1) + margin(1)
    int box_h = count + 2; // rows + top/bottom border

    int origin_x, origin_y;
    popupCenter(box_w, box_h, &origin_x, &origin_y);

    PopupStyle style;
    popupStyleInit(&style);

    popupDrawTop(ab, &style, origin_x, origin_y, box_w);

    // ── rows: second pass, walking the list again to draw ──────────────────
    int row = 0;
    b = E.buffers;
    do {
        int is_sel = (b->buffer_id == E.buffer_switcher.current_buffer_id);

        popupRowBegin(ab, &style, origin_x, origin_y + 1 + row, is_sel);

        char line[160];
        formatBufferLine(b, line, sizeof(line));
        char padded[160];
        snprintf(padded, sizeof(padded), "%-*s", content_w, line);
        padded[content_w] = '\0'; // truncate if the line itself overruns content_w
        abAppend(ab, padded, content_w);

        popupRowEnd(ab, &style);

        row++;
        b = b->next;
    } while (b->buffer_id != first_buffer_id);

    popupDrawBottom(ab, &style, origin_x, origin_y + 1 + count, box_w);
    popupDrawShadow(ab, &style, origin_x, origin_y, box_w, box_h);
}
