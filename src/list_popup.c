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
#include "../include/popup_ui.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

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
    if (content_w > E.screencols - 6) content_w = E.screencols - 6;
    if (content_w > LIST_POPUP_MAX_WIDTH) content_w = LIST_POPUP_MAX_WIDTH;
    if (content_w < 10) content_w = 10;

    int box_w = content_w + 4; // border(2) + marker/margin(1) + margin(1)
    int box_h = visible_count + 4; // top border, title, separator, rows, bottom border

    int origin_x, origin_y;
    popupCenter(box_w, box_h, &origin_x, &origin_y);

    PopupStyle style;
    popupStyleInit(&style);

    popupDrawTop(ab, &style, origin_x, origin_y, box_w);
    popupDrawTitleRow(ab, &style, origin_x, origin_y + 1, box_w, E.list_popup.title, content_w);
    popupDrawSeparator(ab, &style, origin_x, origin_y + 2, box_w);

    it = lisp_list_advance(E.list_popup.items, scroll);
    for (int i = 0; i < visible_count; i++) {
        int idx = i + scroll;
        int is_sel = (idx == E.list_popup.selected);
        int row = origin_y + 3 + i;

        popupRowBegin(ab, &style, origin_x, row, is_sel);

        const char *label = "";
        if (lisp_is_pair(it)) {
            Lisp pair = lisp_car(it);
            if (lisp_is_pair(pair)) label = lisp_string(lisp_car(pair));
        }
        char label_col[LIST_POPUP_MAX_WIDTH + 1];
        snprintf(label_col, sizeof(label_col), "%-*s", content_w, label);
        label_col[content_w] = '\0'; // truncate if the label overruns content_w
        abAppend(ab, label_col, content_w);

        popupRowEnd(ab, &style);

        if (lisp_is_pair(it)) it = lisp_cdr(it);
    }

    popupDrawBottom(ab, &style, origin_x, origin_y + 3 + visible_count, box_w);
    popupDrawShadow(ab, &style, origin_x, origin_y, box_w, box_h);
}
