// include/term_render.h
#ifndef TERM_RENDER_H_
#define TERM_RENDER_H_
#include "data.h"
// Renders one terminal row's cells (with their colours/attributes) as an
// escape-sequence string, clipped to the window [x_offset, x_offset+width)
// — the same clipping the file-buffer paths get from highlight_line(). A
// terminal row is as wide as the PTY was when the shell wrote it, which is
// not necessarily as wide as the pane showing it now: split the screen
// after opening a terminal and its rows are suddenly twice the pane's
// width. Without the clip those cells are simply emitted, running past the
// pane's right edge into the next pane (or off-screen, where the real
// terminal wraps them onto the following line and corrupts the frame).
char* termRenderRow(const Row* row, int* length, int x_offset, int width);
#endif
