#ifndef SCREEN_DIFF_H_
#define SCREEN_DIFF_H_

// Line-diffed screen output (screen_diff.c). editorRefreshScreen() still
// builds a whole frame every tick; screenDiffWrite() replays it onto a
// copy of what the terminal shows and writes only the rows that changed,
// then the cursor. Anything it doesn't model falls back to writing the
// frame as is.

// Writes `frame` (`len` bytes, drawn from the top-left) for a terminal of
// `rows` x `cols`.
void screenDiffWrite(const char *frame, int len, int rows, int cols);
// Forgets what the terminal shows, so the next frame is written in full
// (after something else drew on it, or to repaint on request).
void screenDiffInvalidate(void);
// Frees the saved screen (at exit).
void screenDiffFree(void);
// Trades what the terminal shows for `*other`, the copy a daemon window
// kept while another was drawn (NULL: a window never drawn yet, which is
// repainted in full). The current one is left in `*other`.
void screenDiffSwap(void **other);
// Frees a copy screenDiffSwap() left in `*other` (a window closing).
void screenDiffFreeSaved(void *saved);

#endif // SCREEN_DIFF_H_
