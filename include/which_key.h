//
// which-key style discoverability help.
//

#ifndef BELUGA_WHICH_KEY_H
#define BELUGA_WHICH_KEY_H

#include "data.h"

// Draws the bottom help panel, if anything is worth showing right now:
//   - while the M-x prompt is up, the commands matching what's been typed;
//   - while a key prefix is pending (e.g. after CTRL-x), the keys available
//     under it and what they run.
// A no-op otherwise. Call from editorRefreshScreen() after the panes are
// drawn, so it overlays them.
void whichKeyDraw(struct abuf *ab);

// (Re)build the M-x candidate list from the current Lisp environment.
// Called when the M-x prompt opens rather than per-frame: it walks the
// global env into an alist, which allocates Lisp memory, and the panel is
// redrawn ~20x/second.
void whichKeyBuildCommandList(void);
void whichKeyFreeCommandList(void);

// Drop the cached key-sequence -> command-name mapping, so it's rebuilt on
// the next draw. Call after keybindings change at runtime.
void whichKeyInvalidate(void);
// The printable command name of E.key_binds[index] ("editor-save",
// "editor-set-prefix user", "?" when it can't be named), from the same
// cache the panel uses. NULL for an index out of range.
const char *whichKeyBindingName(int index);
// The symbol a function value is bound to globally, or NULL. Walks the
// environment: for one-off lookups, not per frame.
const char *whichKeyCommandName(Lisp value);

#endif // BELUGA_WHICH_KEY_H
