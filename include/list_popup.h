#ifndef BELUGA_LIST_POPUP_H
#define BELUGA_LIST_POPUP_H

#include "data.h"
#include "append_buffer.h"

// Opens the popup with `title`, a proper Lisp list of (label . value)
// pairs, and callbacks invoked on selection/cancellation. `on_select` is
// called with the chosen pair's cdr as its single argument; `on_cancel`
// (may be lisp_null()) is called with no arguments if the user presses
// ESC instead. Does nothing if `items` is empty.
//
// `filterable`: when true, printable keys and BACKSPACE edit a live query
// shown in the title bar, and the visible/navigable list narrows to
// pairs whose label contains it (case-insensitive substring) — the arrow
// keys, ENTER and ESC all still act on the filtered view. When false,
// behaves exactly as before (every key not already bound above is
// swallowed, not typed into a query).
void listPopupOpen(const char *title, Lisp items, Lisp on_select, Lisp on_cancel, int filterable);

// Input handling — returns 1 if the key was consumed by the popup.
// Call this before normal keybind dispatch while the popup is visible.
int listPopupHandleKey(Utf8Char *key);

// Call from editorRefreshScreen() — appends the popup to abuf when visible.
void listPopupDraw(struct abuf *ab);

#endif //BELUGA_LIST_POPUP_H
