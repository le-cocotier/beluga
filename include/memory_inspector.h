#ifndef MEMORY_INSPECTOR_H_
#define MEMORY_INSPECTOR_H_

// ─── Memory inspector ───────────────────────────────────────────────────────
// Debug-only (APP_DEBUG) view of every block currently tracked by gutils
// (galloc/grealloc). Results live in an ordinary read-only scratch buffer
// ("*memory*"), same as project search and the explorer sidebar, so it
// scrolls and splits like any other buffer.

// Opens (or reuses) the "*memory*" buffer, fills it with a fresh snapshot
// of every tracked block, and shows it in the active pane.
void memoryInspectorOpen(void);

// Re-renders the snapshot into the already-open "*memory*" buffer without
// changing pane focus. No-op if the buffer isn't open.
void memoryInspectorRefresh(void);

// Cycles the display sort order (recency -> size, largest first ->
// file:line/func location -> back to recency) and re-renders. The
// underlying gutils list itself is never reordered.
void memoryInspectorCycleSort(void);

void memoryReport(void);

// Prompts for a file path (ESC cancels) and writes the current sorted
// snapshot to it as plain text.
void memoryInspectorExport(void);

#endif
