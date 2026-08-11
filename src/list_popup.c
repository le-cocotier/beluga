/**
 * @file list_popup.c
 * @brief Generic modal list picker driven entirely from Lisp via
 * (editor-list-popup title items on-select on-cancel).
 * @details A single global instance (E.list_popup), same shape as the
 * CTRL-x b buffer switcher (buffer_switcher.c) and the LSP completion
 * popup (lsp_ui.c), but with no notion of buffers or completion items —
 * just (label . value) pairs supplied by whichever Lisp package opened it.
 * This is the primitive packages like the outline viewer build on instead
 * of every feature growing its own bespoke C popup.
 */

#include "../include/list_popup.h"
#include "../include/terminal.h"
#include "../include/define.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

// ─── ANSI helpers (same small private set buffer_switcher.c/lsp_ui.c keep for themselves) ──

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

void listPopupOpen(const char *title, Lisp items, Lisp on_select, Lisp on_cancel) {
    strncpy(E.list_popup.title, title, sizeof(E.list_popup.title) - 1);
    E.list_popup.title[sizeof(E.list_popup.title) - 1] = '\0';
    E.list_popup.items = items;
    E.list_popup.count = lisp_list_length(items);
    E.list_popup.selected = 0;
    E.list_popup.on_select = on_select;
    E.list_popup.on_cancel = on_cancel;
    // Nothing to show and nothing to navigate — leave it closed rather
    // than drawing an empty box.
    E.list_popup.visible = E.list_popup.count > 0;
}

int listPopupHandleKey(Utf8Char *key) {
    if (!E.list_popup.visible)
        return 0;

    unsigned char c = key->character[0];
    int is_single = (key->length == 1);

    if (is_single && c == '\x1b') { // ESC — cancel
        E.list_popup.visible = 0;
        if (!lisp_is_null(E.list_popup.on_cancel)) {
            LispError err = LISP_ERROR_NONE;
            lisp_apply(E.list_popup.on_cancel, lisp_null(), &err, E.ctx);
        }
        return 1;
    }
    if (is_single && (c == ARROW_DOWN || c == CTRL_KEY('n'))) {
        if (E.list_popup.selected < E.list_popup.count - 1)
            E.list_popup.selected++;
        return 1;
    }
    if (is_single && (c == ARROW_UP || c == CTRL_KEY('p'))) {
        if (E.list_popup.selected > 0)
            E.list_popup.selected--;
        return 1;
    }
    if (is_single && c == '\r') { // ENTER — select the highlighted item
        Lisp item = lisp_list_ref(E.list_popup.items, E.list_popup.selected);
        Lisp value = lisp_is_pair(item) ? lisp_cdr(item) : lisp_null();
        E.list_popup.visible = 0;
        if (!lisp_is_null(E.list_popup.on_select)) {
            LispError err = LISP_ERROR_NONE;
            lisp_apply(E.list_popup.on_select,
                       lisp_cons(value, lisp_null(), E.ctx), &err, E.ctx);
        }
        return 1;
    }

    // This is a modal picker, not a live-filtering popup like completion —
    // stray keys shouldn't leak through into whatever buffer was active
    // before it opened, so just swallow them.
    return 1;
}

void listPopupDraw(struct abuf *ab) {
    if (!E.list_popup.visible || E.list_popup.count == 0)
        return;

    int count = E.list_popup.count;
    int visible_count = count < LIST_POPUP_MAX_H ? count : LIST_POPUP_MAX_H;

    // Scroll window so the selected item is always visible.
    int scroll = 0;
    if (E.list_popup.selected >= LIST_POPUP_MAX_H)
        scroll = E.list_popup.selected - LIST_POPUP_MAX_H + 1;

    // Size the box to the longest visible label (or the title, if wider).
    int content_w = (int) strlen(E.list_popup.title);
    Lisp it = lisp_list_advance(E.list_popup.items, scroll);
    for (int i = 0; i < visible_count && lisp_is_pair(it); i++, it = lisp_cdr(it)) {
        Lisp pair = lisp_car(it);
        if (!lisp_is_pair(pair)) continue;
        int label_len = (int) strlen(lisp_string(lisp_car(pair)));
        if (label_len > content_w) content_w = label_len;
    }
    if (content_w > E.screencols - 4) content_w = E.screencols - 4;
    if (content_w > LIST_POPUP_MAX_WIDTH) content_w = LIST_POPUP_MAX_WIDTH;
    if (content_w < 10) content_w = 10;

    int box_w = content_w + 4; // "| " + text + " |"
    int box_h = visible_count + 4; // top border, title, separator, rows, bottom border

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

    // ── title ────────────────────────────────────────────────────────────────
    ab_move(ab, origin_y + 1, origin_x);
    ab_sgr(ab, "1;37;40");
    abAppend(ab, "| ", 2);
    char title_col[LIST_POPUP_MAX_WIDTH + 1];
    snprintf(title_col, sizeof(title_col), "%-*s", content_w, E.list_popup.title);
    title_col[content_w] = '\0';
    abAppend(ab, title_col, content_w);
    abAppend(ab, " |", 2);
    AB_RESET(ab);

    // ── separator ────────────────────────────────────────────────────────────
    ab_move(ab, origin_y + 2, origin_x);
    ab_sgr(ab, "0;37;40");
    abAppend(ab, "├", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┤", 3);

    // ── rows ─────────────────────────────────────────────────────────────────
    it = lisp_list_advance(E.list_popup.items, scroll);
    for (int i = 0; i < visible_count; i++) {
        int idx = i + scroll;
        int is_sel = (idx == E.list_popup.selected);

        ab_move(ab, origin_y + 3 + i, origin_x);
        ab_sgr(ab, is_sel ? "7" : "0;37;40");
        abAppend(ab, "| ", 2);

        const char *label = "";
        if (lisp_is_pair(it)) {
            Lisp pair = lisp_car(it);
            if (lisp_is_pair(pair)) label = lisp_string(lisp_car(pair));
        }
        char label_col[LIST_POPUP_MAX_WIDTH + 1];
        snprintf(label_col, sizeof(label_col), "%-*s", content_w, label);
        label_col[content_w] = '\0'; // truncate if the label overruns content_w
        abAppend(ab, label_col, content_w);

        abAppend(ab, " |", 2);
        AB_RESET(ab);

        if (lisp_is_pair(it)) it = lisp_cdr(it);
    }

    // ── bottom border ───────────────────────────────────────────────────────
    ab_move(ab, origin_y + 3 + visible_count, origin_x);
    ab_sgr(ab, "0;37;40");
    abAppend(ab, "└", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┘", 3);
    AB_RESET(ab);
}
