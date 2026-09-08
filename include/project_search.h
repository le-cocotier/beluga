#ifndef PROJECT_SEARCH_H_
#define PROJECT_SEARCH_H_

// ─── Project-wide search ────────────────────────────────────────────────────
// Grep the whole project, put the hits in a buffer, and jump from a hit to
// the place in the file it came from. The results buffer is an ordinary
// read-only scratch buffer (EXPLORER_BUFF), so it scrolls, splits and shows
// up in the buffer list like anything else — the only special thing about
// it is that focusing it activates the "search" prefix, where ENTER means
// "go to the result under the cursor".

// Prompts for a pattern, searches from the project root down, fills the
// "*search*" buffer with the hits and shows it in the active pane.
void projectSearchPrompt(void);

// Runs a search without prompting (same result buffer and display).
// `root` may be NULL, meaning the process's working directory.
// Returns the number of matches, or -1 if the search itself failed.
int projectSearchRun(const char *pattern, const char *root);

// Jumps to whatever result the cursor is sitting on in the results buffer.
// A no-op on a header row, on a stale row (the results were replaced by a
// newer search), or when the active buffer isn't the results buffer.
// Opens the file in the *other* pane when the screen is split, so the
// results stay visible; otherwise in place.
void projectSearchGotoCursor(void);

// Frees the stored results. Called on shutdown.
void projectSearchFree(void);

#endif
