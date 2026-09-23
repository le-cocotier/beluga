#ifndef BELUGA_BUFFER_SWITCHER_H
#define BELUGA_BUFFER_SWITCHER_H

#include "data.h"
#include "append_buffer.h"

// Opens the popup, starting on whichever buffer is currently active.
void bufferSwitcherOpen(void);

// Input handling — returns 1 if the key was consumed by the popup.
// Call this before normal keybind dispatch while the popup is visible.
int bufferSwitcherHandleKey(const Utf8Char *key);

// Call from editorRefreshScreen() — appends the popup to abuf when visible.
void bufferSwitcherDraw(struct abuf *ab);

#endif //BELUGA_BUFFER_SWITCHER_H
