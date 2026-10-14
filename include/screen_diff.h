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

#endif // SCREEN_DIFF_H_
