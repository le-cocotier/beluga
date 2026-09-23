#ifndef BELUGA_FILE_FINDER_H
#define BELUGA_FILE_FINDER_H

#include "data.h"
#include "append_buffer.h"

// Opens the fuzzy file picker: snapshots every file in the project (git
// ls-files inside a repo, a pruned find(1) otherwise), rooted at the
// editor's working directory, and shows them in a modal popup that
// narrows as you type. Does nothing (just a status message) if no files
// are found. Bound to whatever the config maps to `find-file`.
void fileFinderOpen(void);

// Frees the path snapshot and hides the popup. Safe to call when already
// closed; called from deInitEditor() too.
void fileFinderClose(void);

// Input handling — returns 1 if the key was consumed. Call before normal
// keybind dispatch while the popup is visible (it's modal, like the
// buffer switcher and the list popup).
int fileFinderHandleKey(const Utf8Char *key);

// Call from editorRefreshScreen() — appends the popup to `ab` when visible.
void fileFinderDraw(struct abuf *ab);

#endif // BELUGA_FILE_FINDER_H
