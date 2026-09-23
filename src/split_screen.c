/**
 * @file split_screen.c
 * @brief Split screen implementation
 */

#include "../include/split_screen.h"
#include "../include/galloc.h"
#include "../include/buffer.h"
#include "../include/editor_op.h"
#include "../include/pty_session.h"
#include "include/builtins.h"
#include "include/data.h"
#include <stdlib.h>
#include <string.h>


extern EditorConfig E;

/**
 * @brief Initializes split screen system with single fullscreen pane
 * @note Allocate dynamically : \n
 *   - E.panes.layout
 */
void splitScreenInit(void) {
    EditorPane *new_panes;
    E.layout.mode = SPLIT_NONE;
    E.layout.num_panes = 1;
    E.layout.active_pane = 0;

    new_panes = grealloc(E.layout.panes, sizeof(EditorPane) * 2);
    if (!new_panes) {
        editorSetStatusMessage("Error: realloc failed");
        return;
    }
    E.layout.panes = new_panes;

    // Initialize single fullscreen pane
    E.layout.panes[0].buffer_id = -1; // No buffer for now
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].height = E.screenrows; // already excludes the 2 status rows
    E.layout.panes[0].width = E.screencols;
    E.layout.panes[0].cursor_x = 0;
    E.layout.panes[0].cursor_y = 0;
    E.layout.panes[0].y_offset = 0;
    E.layout.panes[0].x_offset = 0;
    E.layout.panes[0].is_active = 1;
}

/**
 * @brief The key prefix a buffer wants active while it has focus.
 * @details Scratch buffers can carry their own keymap (the explorer
 * sidebar's arrows and ENTER, the search results buffer's ENTER-to-jump).
 * Which one is a property of the buffer, not of its type — both are
 * EXPLORER_BUFFs — so the name is read off the buffer, falling back to
 * "explorer" for every scratch buffer created before this field existed.
 */
static const char *bufferFocusPrefix(const struct buffer_t *buf) {
    if (buf && buf->focus_prefix[0])
        return buf->focus_prefix;
    return "explorer";
}

/**
 * @brief Puts the key prefix in the state the now-focused pane's buffer
 * expects.
 * @details Landing in a terminal pane has to switch to "term", or the
 * no-prefix bindings (arrows, ENTER, CTRL-a...) match first in
 * executeKeyBind() and are consumed as editor commands instead of falling
 * through to ptyForwardKey() and reaching the shell. A scratch buffer
 * likewise wants its own keymap. Anything else must land back in
 * no-prefix, or a "term"/"explorer" prefix left over from the pane you
 * came from silently swallows every keystroke here — nothing is mapped
 * under it, so nothing matches.
 *
 * Shared by pane switching and by splitting, which both change which pane
 * has focus: a split of a terminal pane used to leave the new (still
 * terminal) pane in whatever prefix was active, with the shell unable to
 * see the arrow keys.
 */
static void splitScreenApplyFocusPrefix(void) {
    const Buffer *buf = bufferFindById(E.layout.panes[E.layout.active_pane].buffer_id);
    const char *name = NULL;
    struct prefix_t prefix;

    if (buf && buf->type == TERMINAL_BUFF)
        name = "term";
    else if (buf && buf->type == EXPLORER_BUFF)
        name = bufferFocusPrefix(buf);

    if (name) {
        prefix = find_prefix(name);
        if (prefix.prefix_id != -1)
            E.prefix_state = prefix.prefix_id;
    } else {
        E.prefix_state = 0;
    }
}

/**
 * @brief Splits the *active* pane in two, in `direction`.
 * @details Replaces the old splitScreenVertical()/splitScreenHorizontal()
 * pair, which each rebuilt the whole layout as exactly two panes taken
 * from two specific buffer ids. That capped the editor at two panes no
 * matter what, and only ever paired the head of the buffer list with
 * whatever happened to sit behind it.
 *
 * Splitting the active pane's own rectangle instead composes without a
 * limit: every pane already carries its own origin/width/height, so
 * halving one and handing the other half to a new pane subdivides the
 * screen to any depth — split left/right, focus the right half, split it
 * top/bottom, and so on. No pane tree is needed for that; the rectangles
 * alone describe the layout, and the dividers are drawn from the gaps
 * between them (see editorDrawAllPanes()).
 *
 * One column (or row) is deliberately left unclaimed between the two
 * halves for the divider to live in, so it no longer paints over the last
 * column of the pane to its left the way the old fixed layout did.
 *
 * The new pane shows the same buffer as the one it was split from — the
 * predictable thing every other editor does, and the buffer is one
 * CTRL-x ARROW-RIGHT (or CTRL-x b) away from being changed. Focus moves
 * to the new pane, since the reason to split is almost always to put
 * something else in the half you just made.
 * @return 0 on success, -1 if the pane is too small to divide.
 */
int splitScreenSplitActive(const SplitMode direction) {
    // Below this a pane has no room for a gutter plus readable text, and
    // halving it repeatedly would eventually produce zero/negative sizes.
    const int MIN_WIDTH  = 20;
    const int MIN_HEIGHT = 4;

    EditorPane *active = &E.layout.panes[E.layout.active_pane];
    EditorPane *grown;
    EditorPane *fresh;
    int left;
    int top;

    if (direction == SPLIT_VERTICAL && active->width < MIN_WIDTH * 2 + 1) {
        editorSetStatusMessage("Pane too narrow to split");
        return -1;
    }
    if (direction == SPLIT_HORIZONTAL && active->height < MIN_HEIGHT * 2 + 1) {
        editorSetStatusMessage("Pane too short to split");
        return -1;
    }

    grown = grealloc(E.layout.panes,
                                sizeof(EditorPane) * (size_t)(E.layout.num_panes + 1));
    if (!grown) {
        editorSetStatusMessage("Error: realloc failed");
        return -1;
    }
    E.layout.panes = grown;
    // realloc may have moved the array — re-derive rather than reusing the
    // pointer taken above it.
    active = &E.layout.panes[E.layout.active_pane];

    fresh = &E.layout.panes[E.layout.num_panes];
    *fresh = *active;              // inherit buffer and geometry, then carve
    fresh->cursor_x = 0;
    fresh->cursor_y = 0;
    fresh->x_offset = 0;
    fresh->y_offset = 0;

    if (direction == SPLIT_VERTICAL) {
        left = (active->width - 1) / 2;   // -1 for the divider column
        fresh->origin_x = active->origin_x + left + 1;
        fresh->width    = active->width - left - 1;
        active->width   = left;
    } else {
        top = (active->height - 1) / 2;   // -1 for the divider row
        fresh->origin_y = active->origin_y + top + 1;
        fresh->height   = active->height - top - 1;
        active->height  = top;
    }

    active->is_active = 0;
    fresh->is_active = 1;
    E.layout.active_pane = E.layout.num_panes;
    E.layout.num_panes++;
    E.layout.mode = direction;
    splitScreenApplyFocusPrefix();

    // The active pane's cursor may now sit outside its narrower/shorter
    // rectangle; editorScroll() pulls it back on the next redraw, but the
    // PTY behind a terminal buffer has to be told directly.
    splitScreenSyncTerminalSizes();

    editorSetStatusMessage("Split %s — %d panes",
                           direction == SPLIT_VERTICAL ? "vertically" : "horizontally",
                           E.layout.num_panes);
    return 0;
}

/**
 * @brief Returns to single buffer fullscreen
 * @details Prefers a real (non-EXPLORER_BUFF) pane's buffer as what
 * survives the collapse. panes[0] is otherwise just "whichever pane
 * happens to be first" — and the sidebar always sits at panes[0] (see
 * splitScreenWithSidebar()), so blindly keeping it would leave the
 * read-only tree fullscreen and silently drop whatever real content was
 * in the other pane. This used to be a search splitScreenCloseSidebar()
 * ran itself before calling this; moved here because that isn't the only
 * way to reach a collapse-to-one-pane with the sidebar still open — ESC
 * out of the sidebar's own keymap back to no-prefix first (its focus
 * doesn't move off the sidebar pane), then a plain CTRL-x * reaches this
 * function directly, and used to strand you looking at *explorer* with
 * no visible way back to what you were editing.
 *
 * Also resyncs E.prefix_state to whatever buffer actually survives (see
 * splitScreenApplyFocusPrefix()) — this function used to leave the prefix
 * exactly as it was before the collapse, no matter what. Landing on a
 * terminal buffer while the prefix was still e.g. "no-prefix" is the
 * dangerous case: ENTER/BACKSPACE/typing would then run as ordinary
 * buffer-editing calls (bufferInsertNewLine, bufferRowInsertChar, ...)
 * directly on that row instead of being forwarded to the PTY. Those know
 * nothing about a terminal row's attrs[] (an ANSI-parser-only concept —
 * see ansi_parser.c's ensureAttrs()) and grow row->size/characters
 * without it, so a later termRenderRow() call reads attrs[] past its
 * actual allocation — a real heap-buffer-overflow, not just a display
 * glitch, reproduced via exactly this sequence (open a terminal, land on
 * it through a collapse with the wrong prefix, then edit).
 */
void splitScreenUnify(void) {
    int keep_buffer_id = E.layout.panes[0].buffer_id;
    int i;
    struct buffer_t *b;
    for (i = 0; i < E.layout.num_panes; i++) {
        b = bufferFindById(E.layout.panes[i].buffer_id);
        if (b && b->type != EXPLORER_BUFF) {
            keep_buffer_id = E.layout.panes[i].buffer_id;
            break;
        }
    }

    E.layout.mode = SPLIT_NONE;
    E.layout.num_panes = 1;
    E.layout.active_pane = 0;

    E.layout.panes[0].buffer_id = keep_buffer_id;
    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].height = E.screenrows;
    E.layout.panes[0].width = E.screencols;
    E.layout.panes[0].is_active = 1;

    splitScreenApplyFocusPrefix();
    editorSetStatusMessage("Unified view");
    splitScreenSyncTerminalSizes();
}

/**
 * @brief Tells every terminal buffer currently in a pane how big its pane
 * is now.
 * @details A PTY is sized once, when the shell is spawned, from whatever
 * pane it was opened in. Every later change to the layout — splitting,
 * unifying, opening or closing the sidebar, resizing the window — moves
 * that pane's edges without the shell hearing about it, so it keeps
 * wrapping its output to the old width. The rows it produces are then
 * wider than the pane they're drawn in (terminal rows are clipped at
 * render time, so this shows up as truncation rather than as text spilling
 * into the neighbouring pane, but the shell is still laying out for a
 * screen that isn't there). Call this after anything that changes pane
 * geometry.
 * @note Walks the panes rather than the buffer list on purpose: a terminal
 * buffer that isn't currently displayed has no size to be resized to, and
 * SIGWINCH previously only ever resized the *active* pane's terminal —
 * so a terminal sitting in the other half of a split silently kept a stale
 * size for the rest of the session.
 */
void splitScreenSyncTerminalSizes(void) {
    const ScreenLayout *layout = &E.layout;
    int i;
    EditorPane *pane;
    struct buffer_t *buf;
    for (i = 0; i < layout->num_panes; i++) {
        pane = &layout->panes[i];
        buf = bufferFindById(pane->buffer_id);
        if (!buf || buf->type != TERMINAL_BUFF || buf->pty_master_fd < 0)
            continue;
        ptySessionResize(buf, pane->height, pane->width);
        buf->term_state.pane_height = pane->height;
    }
}


/**
 * @brief Splits the screen vertically with a fixed-width left pane
 * (e.g. an explorer sidebar) and the rest of the screen on the right.
 */
int splitScreenWithSidebar(const int sidebar_buffer_id, const int main_buffer_id, int sidebar_width) {
    int pane_height;
    struct prefix_t prefix;
    if (bufferFindById(sidebar_buffer_id) == NULL ||
        bufferFindById(main_buffer_id) == NULL) {
        editorSetStatusMessage("Error: invalid buffer IDs");
        return -1;
    }

    E.layout.panes = grealloc(E.layout.panes, sizeof(EditorPane) * 2);
    E.layout.mode = SPLIT_VERTICAL;
    E.layout.num_panes = 2;
    E.layout.active_pane = 0;

    if (sidebar_width < 1) sidebar_width = 1;
    if (sidebar_width > E.screencols - 1) sidebar_width = E.screencols - 1;
    pane_height = E.screenrows;

    // Sidebar pane
    E.layout.panes[0].buffer_id = sidebar_buffer_id;
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].height = pane_height;
    E.layout.panes[0].width = sidebar_width;
    E.layout.panes[0].cursor_x = 0;
    E.layout.panes[0].cursor_y = 0;
    E.layout.panes[0].y_offset = 0;
    E.layout.panes[0].x_offset = 0;
    E.layout.panes[0].is_active = 1;

    // Main pane
    E.layout.panes[1].buffer_id = main_buffer_id;
    E.layout.panes[1].origin_y = 0;
    E.layout.panes[1].origin_x = sidebar_width;
    E.layout.panes[1].height = pane_height;
    E.layout.panes[1].width = E.screencols - sidebar_width;
    E.layout.panes[1].cursor_x = 0;
    E.layout.panes[1].cursor_y = 0;
    E.layout.panes[1].y_offset = 0;
    E.layout.panes[1].x_offset = 0;
    E.layout.panes[1].is_active = 0;

    // Focus starts on the sidebar — the whole point of just having opened
    // it is to pick something in it.
    prefix = find_prefix(bufferFocusPrefix(bufferFindById(sidebar_buffer_id)));
    E.prefix_state = prefix.prefix_id;

    splitScreenSyncTerminalSizes();
    editorSetStatusMessage("Explorer opened");
    return 0;
}

/**
 * @brief Collapses a sidebar split back to fullscreen, keeping whichever
 * pane ISN'T an EXPLORER_BUFF visible.
 * @note Both the "keep a real pane, not the sidebar" search and the
 * prefix resync now live in splitScreenUnify() itself — every path that
 * collapses to one pane goes through it, not just this one. Kept as its
 * own named entry point for callers that mean specifically "close the
 * sidebar" (editor-explorer-close), even though it no longer does
 * anything splitScreenUnify() doesn't already do on its own. This used
 * to force E.prefix_state to 0 unconditionally here — wrong the moment
 * the pane that survives is a terminal, which needs "term", not
 * no-prefix (see splitScreenUnify()'s own doc comment for why that
 * particular mismatch is a real memory-safety bug, not just a UX one).
 */
void splitScreenCloseSidebar(void) {
    splitScreenUnify();
}

/**
 * @brief Moves focus to some other pane that isn't a sidebar-style
 * EXPLORER_BUFF, without touching pane count or geometry.
 * @details For "open this into the other pane" callers (project-search's
 * jump-to-result is the first one) that want a real content pane to land
 * in, not just splitScreenSwitchPane()'s blind next-in-rotation — which
 * happily lands on the explorer sidebar if one happens to be part of the
 * current split, silently overwriting it with whatever the caller was
 * about to show. Prefers the pane right after the current one, then wraps
 * around; does nothing (and returns 0) if every other pane is a sidebar
 * (or there's only one pane to begin with) — callers fall back to opening
 * in place, same as they already do when there's no split at all.
 * @return 1 if focus moved to a different pane, 0 if it stayed put.
 */
int splitScreenFocusOtherRealPane(void) {
    int start;
    int step;
    int i;
    struct buffer_t *b;
    if (E.layout.num_panes < 2)
        return 0;

    start = E.layout.active_pane;
    for (step = 1; step < E.layout.num_panes; step++) {
        i = (start + step) % E.layout.num_panes;
        b = bufferFindById(E.layout.panes[i].buffer_id);
        if (b && b->type == EXPLORER_BUFF)
            continue;

        E.layout.panes[start].is_active = 0;
        E.layout.active_pane = i;
        E.layout.panes[i].is_active = 1;
        splitScreenApplyFocusPrefix();
        return 1;
    }
    return 0;
}

/**
 * @brief Switches active pane (focus moves between splits)
 * @return 0 on success, -1 on failure
 */
int splitScreenSwitchPane(void) {
    if (E.layout.num_panes < 2) {
        editorSetStatusMessage("No split to switch");
        return -1;
    }

    // Deactivate current pane
    E.layout.panes[E.layout.active_pane].is_active = 0;

    // Move to next pane
    E.layout.active_pane = (E.layout.active_pane + 1) % E.layout.num_panes;
    splitScreenApplyFocusPrefix();

    // Activate new pane
    E.layout.panes[E.layout.active_pane].is_active = 1;

    editorSetStatusMessage("Switched to pane %d (buffer %d)",
                          E.layout.active_pane,
                          E.layout.panes[E.layout.active_pane].buffer_id);
    return 0;
}

/**
 * @brief Updates the active pane's buffer
 * @param buffer_id New buffer ID for active pane
 * @return 0 on success, -1 on failure
 */
int splitScreenSetPaneBuffer(const int buffer_id) {
    EditorPane *active;
    if (bufferFindById(buffer_id) == NULL) {
        editorSetStatusMessage("Error: invalid buffer ID");
        return -1;
    }

    active = &E.layout.panes[E.layout.active_pane];
    active->buffer_id = buffer_id;
    active->cursor_x = 0;
    active->cursor_y = 0;
    active->origin_x = 0;
    active->origin_y = 0;

    editorSetStatusMessage("Pane %d now showing buffer %d",
                          E.layout.active_pane, buffer_id);
    return 0;
}

/**
 * @brief Gets current screen layout
 * @return Pointer to current ScreenLayout
 */
ScreenLayout *splitScreenGetLayout(void) {
    return &E.layout;
}

/**
 * @brief Gets active pane
 * @return Pointer to active EditorPane
 */
EditorPane *splitScreenGetActivePane(void) {
    if (E.layout.num_panes == 0) return NULL;
    return &E.layout.panes[E.layout.active_pane];
}

void freeScreenLayout(ScreenLayout *layout) {
    if (layout) {
        gfree(layout->panes);
        layout->panes = NULL;
    }
}

void freePane(EditorPane *pane)
{
    gfree(pane);
}
