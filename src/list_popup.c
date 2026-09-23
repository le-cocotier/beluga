/**
 * @file list_popup.c
 * @brief Generic modal list picker driven entirely from Lisp via
 * (editor-list-popup title items on-select on-cancel filterable?).
 * @details A single global instance (E.list_popup), same shape as the
 * CTRL-x b buffer switcher (buffer_switcher.c) and the LSP completion
 * popup (lsp_ui.c), but with no notion of buffers or completion items —
 * just (label . value) pairs supplied by whichever Lisp package opened it.
 * This is the primitive packages like the outline viewer build on instead
 * of every feature growing its own bespoke C popup — and, with the
 * optional `filterable?` argument, the same primitive for "type to
 * narrow this list" popups too, not just modal pick-with-arrows ones.
 */

#include "../include/list_popup.h"
#include "../include/define.h"
#include "../include/popup_ui.h"
#include "../include/terminal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

extern EditorConfig E;

/**
 * @brief Case-insensitive substring test: does `label` contain `query`
 * anywhere? An empty query always matches (so an untouched filterable
 * popup opens showing everything, same order as passed in).
 */
static int labelMatchesQuery(const char *label, const char *query) {
  const char *p;
  const char *a;
  const char *b;
  if (!query[0])
    return 1;

  for (p = label; *p; p++) {
    a = p;
    b = query;
    while (*a && *b &&
           tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      a++;
      b++;
    }
    if (!*b)
      return 1; // walked off the end of query -> matched in full
  }
  return 0;
}

/**
 * @brief Rebuilds E.list_popup.items/count from all_items + query.
 * @details Only meaningful when filterable — non-filterable popups never
 * call this, items IS all_items for their whole lifetime. Builds the
 * filtered list in the same order as all_items (a tail pointer, not the
 * cons-then-reverse trick used elsewhere in this codebase for the same
 * reason list_directory results get sorted in Lisp instead: here the
 * order is meaningful and worth preserving directly).
 */
static void listPopupRebuildFilter(void) {
  Lisp head = lisp_null();
  Lisp tail = lisp_null();
  int count = 0;
  Lisp it;
  Lisp pair;
  Lisp cell;

  for (it = E.list_popup.all_items; lisp_is_pair(it); it = lisp_cdr(it)) {
    pair = lisp_car(it);
    if (!lisp_is_pair(pair))
      continue;
    if (!labelMatchesQuery(lisp_string(lisp_car(pair)), E.list_popup.query))
      continue;

    cell = lisp_cons(pair, lisp_null(), E.ctx);
    if (lisp_is_null(head))
      head = cell;
    else
      lisp_set_cdr(tail, cell);
    tail = cell;
    count++;
  }

  E.list_popup.items = head;
  E.list_popup.count = count;
  if (E.list_popup.selected >= count)
    E.list_popup.selected = count > 0 ? count - 1 : 0;
  if (E.list_popup.selected < 0)
    E.list_popup.selected = 0;
}

void listPopupOpen(const char *title, const Lisp items, const Lisp on_select,
                   const Lisp on_cancel, const int filterable) {
  strncpy(E.list_popup.title, title, sizeof(E.list_popup.title) - 1);
  E.list_popup.title[sizeof(E.list_popup.title) - 1] = '\0';
  E.list_popup.all_items = items;
  E.list_popup.selected = 0;
  E.list_popup.on_select = on_select;
  E.list_popup.on_cancel = on_cancel;
  E.list_popup.filterable = filterable;
  E.list_popup.query[0] = '\0';
  E.list_popup.query_len = 0;

  if (filterable) {
    listPopupRebuildFilter(); // query is empty here: matches everything, in
                              // order
  } else {
    E.list_popup.items = items;
    E.list_popup.count = lisp_list_length(items);
  }

  // A non-filterable popup with nothing to show has nothing to
  // navigate either — leave it closed rather than drawing an empty
  // box. A filterable one opens even with zero initial items: an empty
  // items list plus a query the caller pre-populated some other way
  // isn't something this builtin does today, but there's no reason to
  // special-case "started empty" out from under a query that could
  // still be typed into productively... in practice this is always
  // non-empty in the filterable case too, since callers pass real data.
  E.list_popup.visible = filterable || E.list_popup.count > 0;
}

int listPopupHandleKey(const Utf8Char *key) {
  unsigned char c;
  int is_single;
  LispError err;
  Lisp item;
  Lisp value;
  int is_text;
  int n;
  if (!E.list_popup.visible)
    return 0;

  c = key->character[0];
  is_single = (key->length == 1);

  if (is_single && c == '\x1b') { // ESC — cancel
    E.list_popup.visible = 0;
    if (!lisp_is_null(E.list_popup.on_cancel)) {
      err = LISP_ERROR_NONE;
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
    if (E.list_popup.count == 0)
      return 1; // filterable + query matches nothing: nothing to select
    item = lisp_list_ref(E.list_popup.items, E.list_popup.selected);
    value = lisp_is_pair(item) ? lisp_cdr(item) : lisp_null();
    E.list_popup.visible = 0;
    if (!lisp_is_null(E.list_popup.on_select)) {
      err = LISP_ERROR_NONE;
      lisp_apply(E.list_popup.on_select, lisp_cons(value, lisp_null(), E.ctx),
                 &err, E.ctx);
    }
    return 1;
  }

  if (E.list_popup.filterable) {
    if (is_single && (c == BACKSPACE || c == DEL_KEY)) {
      if (E.list_popup.query_len > 0) {
        // Step back over a whole UTF-8 char: drop trailing
        // continuation bytes (0b10xxxxxx), then the lead byte.
        E.list_popup.query_len--;
        while (E.list_popup.query_len > 0 &&
               (E.list_popup.query[E.list_popup.query_len] & 0xC0) == 0x80)
          E.list_popup.query_len--;
        E.list_popup.query[E.list_popup.query_len] = '\0';
        E.list_popup.selected = 0;
        listPopupRebuildFilter();
      }
      return 1;
    }

    // Typed text — printable ASCII, or any multi-byte UTF-8 char.
    is_text = (is_single && c >= 0x20 && c <= 0x7E) || key->length > 1;
    if (is_text) {
      n = (unsigned char)key->length;
      if (E.list_popup.query_len + n < (int)sizeof(E.list_popup.query) - 1) {
        memcpy(E.list_popup.query + E.list_popup.query_len, key->character,
               (size_t)n);
        E.list_popup.query_len += n;
        E.list_popup.query[E.list_popup.query_len] = '\0';
        E.list_popup.selected = 0;
        listPopupRebuildFilter();
      }
      return 1;
    }
  }

  // Modal: swallow anything else so it doesn't fall through into the
  // buffer behind it.
  return 1;
}

void listPopupDraw(struct abuf *ab) {
  int count;
  int visible_count;
  int scroll;
  char title[192];
  int content_w;
  Lisp it;
  int i;
  Lisp pair;
  int label_len;
  int box_w;
  int rows;
  int box_h;
  int origin_x;
  int origin_y;
  PopupStyle style;
  char empty[LIST_POPUP_MAX_WIDTH + 1];
  int idx;
  int is_sel;
  int row;
  const char *label;
  char label_col[LIST_POPUP_MAX_WIDTH + 1];
  if (!E.list_popup.visible)
    return;
  // Non-filterable popups never open with count == 0 (see
  // listPopupOpen()); a filterable one legitimately can once a query
  // matches nothing, and still needs to draw — an empty box with
  // "no matches" instead of vanishing until the query changes again.

  count = E.list_popup.count;
  visible_count = count < LIST_POPUP_MAX_H ? count : LIST_POPUP_MAX_H;

  // Scroll window so the selected item is always visible.
  scroll = 0;
  if (E.list_popup.selected >= LIST_POPUP_MAX_H)
    scroll = E.list_popup.selected - LIST_POPUP_MAX_H + 1;

  // Title row: plain title, or "title: query_  (N)" while filterable —
  // same shape find-file's own popup uses, for a consistent feel.
  if (E.list_popup.filterable)
    snprintf(title, sizeof(title), "%s: %s_  (%d)", E.list_popup.title,
             E.list_popup.query, count);
  else
    snprintf(title, sizeof(title), "%s", E.list_popup.title);

  // Size the box to the longest visible label (or the title, if wider).
  content_w = (int)strlen(title);
  it = lisp_list_advance(E.list_popup.items, scroll);
  for (i = 0; i < visible_count && lisp_is_pair(it); i++, it = lisp_cdr(it)) {
    pair = lisp_car(it);
    if (!lisp_is_pair(pair))
      continue;
    label_len = (int)strlen(lisp_string(lisp_car(pair)));
    if (label_len > content_w)
      content_w = label_len;
  }
  if (content_w > E.screencols - 6)
    content_w = E.screencols - 6;
  if (content_w > LIST_POPUP_MAX_WIDTH)
    content_w = LIST_POPUP_MAX_WIDTH;
  if (content_w < 10)
    content_w = 10;

  box_w = content_w + 4; // border(2) + marker/margin(1) + margin(1)
  // At least one content row, so a filterable popup with no current
  // matches still draws a box (for its "no matches" row) instead of
  // collapsing to nothing.
  rows = visible_count > 0 ? visible_count : 1;
  box_h = rows + 4; // top border, title, separator, rows, bottom border

  popupCenter(box_w, box_h, &origin_x, &origin_y);

  popupStyleInit(&style);

  popupDrawTop(ab, &style, origin_x, origin_y, box_w);
  popupDrawTitleRow(ab, &style, origin_x, origin_y + 1, box_w, title,
                    content_w);
  popupDrawSeparator(ab, &style, origin_x, origin_y + 2, box_w);

  if (visible_count == 0) {
    popupRowBegin(ab, &style, origin_x, origin_y + 3, 0);
    snprintf(empty, sizeof(empty), "%-*s", content_w, "  (no matches)");
    empty[content_w] = '\0';
    abAppend(ab, empty, content_w);
    popupRowEnd(ab, &style);
  }

  it = lisp_list_advance(E.list_popup.items, scroll);
  for (i = 0; i < visible_count; i++) {
    idx = i + scroll;
    is_sel = (idx == E.list_popup.selected);
    row = origin_y + 3 + i;

    popupRowBegin(ab, &style, origin_x, row, is_sel);

    label = (const char *)"";
    if (lisp_is_pair(it)) {
      pair = lisp_car(it);
      if (lisp_is_pair(pair))
        label = lisp_string(lisp_car(pair));
    }
    snprintf(label_col, sizeof(label_col), "%-*s", content_w, label);
    label_col[content_w] = '\0'; // truncate if the label overruns content_w
    abAppend(ab, label_col, content_w);

    popupRowEnd(ab, &style);

    if (lisp_is_pair(it))
      it = lisp_cdr(it);
  }

  popupDrawBottom(ab, &style, origin_x, origin_y + 3 + rows, box_w);
  popupDrawShadow(ab, &style, origin_x, origin_y, box_w, box_h);
}
