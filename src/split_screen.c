/**
 * @file split_screen.c
 * @brief Split screen implementation
 */

#include "../include/split_screen.h"
#include "../include/buffer.h"
#include "../include/editor_op.h"
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
    E.layout.mode = SPLIT_NONE;
    E.layout.num_panes = 1;
    E.layout.active_pane = 0;

    EditorPane *new_panes = realloc(E.layout.panes, sizeof(EditorPane) * 2);
    if (!new_panes) {
        editorSetStatusMessage("Error: realloc failed");
        return;
    }
    E.layout.panes = new_panes;

    // Initialize single fullscreen pane
    E.layout.panes[0].buffer_id = -1; // No buffer for now
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].height = E.screenrows - 2;  // Leave room for status bar
    E.layout.panes[0].width = E.screencols;
    E.layout.panes[0].cursor_x = 0;
    E.layout.panes[0].cursor_y = 0;
    E.layout.panes[0].y_offset = 0;
    E.layout.panes[0].x_offset = 0;
    E.layout.panes[0].is_active = 1;
}

/**
 * @brief Splits screen vertically (left-right)
 * @param buffer_id_left Buffer ID for left pane
 * @param buffer_id_right Buffer ID for right pane
 * @return 0 on success, -1 on failure
 */
int splitScreenVertical(int buffer_id_left, int buffer_id_right) {
    // Verify both buffers exist
    if (bufferFindById(buffer_id_left) == NULL ||
        bufferFindById(buffer_id_right) == NULL) {
        editorSetStatusMessage("Error: invalid buffer IDs");
        return -1;
    }

    // bReallocate panes array
    E.layout.panes = realloc(E.layout.panes, sizeof(EditorPane) * 2);
    E.layout.mode = SPLIT_VERTICAL;
    E.layout.num_panes = 2;
    E.layout.active_pane = 0;

    int half_width = E.screencols / 2;
    int pane_height = E.screenrows;  // Leave room for status bar

    // Left pane
    E.layout.panes[0].buffer_id = buffer_id_left;
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].height = pane_height;
    E.layout.panes[0].width = half_width;
    E.layout.panes[0].cursor_x = 0;
    E.layout.panes[0].cursor_y = 0;
    E.layout.panes[0].y_offset = 0;
    E.layout.panes[0].x_offset = 0;
    E.layout.panes[0].is_active = 1;

    Buffer* buf = bufferFindById(buffer_id_left);
    if (buf && buf->type == TERMINAL_BUFF)
    {
        struct prefix_t prefix = find_prefix("term");
        E.prefix_state = prefix.prefix_id;
    }

    // Right pane
    E.layout.panes[1].buffer_id = buffer_id_right;
    E.layout.panes[1].origin_y = 0;
    E.layout.panes[1].origin_x = half_width;
    E.layout.panes[1].height = pane_height;
    E.layout.panes[1].width = E.screencols - half_width;
    E.layout.panes[1].cursor_x = 0;
    E.layout.panes[1].cursor_y = 0;
    E.layout.panes[1].y_offset = 0;
    E.layout.panes[1].x_offset = 0;
    E.layout.panes[1].is_active = 0;

    editorSetStatusMessage("Vertical split: %d | %d", buffer_id_left, buffer_id_right);
    return 0;
}

/**
 * @brief Splits screen horizontally (top-bottom)
 * @param buffer_id_top Buffer ID for top pane
 * @param buffer_id_bottom Buffer ID for bottom pane
 * @return 0 on success, -1 on failure
 */
int splitScreenHorizontal(int buffer_id_top, int buffer_id_bottom) {
    // Verify both buffers exist
    if (bufferFindById(buffer_id_top) == NULL ||
        bufferFindById(buffer_id_bottom) == NULL) {
        editorSetStatusMessage("Error: invalid buffer IDs");
        return -1;
    }

    // bReallocate panes array
    E.layout.panes = realloc(E.layout.panes, sizeof(EditorPane) * 2);
    E.layout.mode = SPLIT_HORIZONTAL;
    E.layout.num_panes = 2;
    E.layout.active_pane = 0;

    int half_height = (E.screenrows - 2) / 2;  // Account for status bar

    // Top pane
    E.layout.panes[0].buffer_id = buffer_id_top;
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].height = half_height;
    E.layout.panes[0].width = E.screencols;
    E.layout.panes[0].cursor_x = 0;
    E.layout.panes[0].cursor_y = 0;
    E.layout.panes[0].y_offset = 0;
    E.layout.panes[0].x_offset = 0;
    E.layout.panes[0].is_active = 1;

    // Bottom pane
    E.layout.panes[1].buffer_id = buffer_id_bottom;
    E.layout.panes[1].origin_y = half_height;
    E.layout.panes[1].origin_x = 0;
    E.layout.panes[1].height = E.screenrows - 2 - half_height;
    E.layout.panes[1].width = E.screencols;
    E.layout.panes[1].cursor_x = 0;
    E.layout.panes[1].cursor_y = 0;
    E.layout.panes[1].x_offset = 0;
    E.layout.panes[1].y_offset = 0;
    E.layout.panes[1].is_active = 0;

    editorSetStatusMessage("Horizontal split: %d / %d", buffer_id_top, buffer_id_bottom);
    return 0;
}

/**
 * @brief Returns to single buffer fullscreen
 */
void splitScreenUnify(void) {
    E.layout.mode = SPLIT_NONE;
    E.layout.num_panes = 1;
    E.layout.active_pane = 0;

    E.layout.panes[0].origin_x = 0;
    E.layout.panes[0].origin_y = 0;
    E.layout.panes[0].height = E.screenrows;
    E.layout.panes[0].width = E.screencols;
    E.layout.panes[0].is_active = 1;

    editorSetStatusMessage("Unified view");
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
 * @brief Splits the screen vertically with a fixed-width left pane
 * (e.g. an explorer sidebar) and the rest of the screen on the right.
 */
int splitScreenWithSidebar(int sidebar_buffer_id, int main_buffer_id, int sidebar_width) {
    if (bufferFindById(sidebar_buffer_id) == NULL ||
        bufferFindById(main_buffer_id) == NULL) {
        editorSetStatusMessage("Error: invalid buffer IDs");
        return -1;
    }

    E.layout.panes = realloc(E.layout.panes, sizeof(EditorPane) * 2);
    E.layout.mode = SPLIT_VERTICAL;
    E.layout.num_panes = 2;
    E.layout.active_pane = 0;

    if (sidebar_width < 1) sidebar_width = 1;
    if (sidebar_width > E.screencols - 1) sidebar_width = E.screencols - 1;
    int pane_height = E.screenrows;

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
    struct prefix_t prefix = find_prefix(bufferFocusPrefix(bufferFindById(sidebar_buffer_id)));
    E.prefix_state = prefix.prefix_id;

    editorSetStatusMessage("Explorer opened");
    return 0;
}

/**
 * @brief Collapses a sidebar split back to fullscreen, keeping whichever
 * pane ISN'T an EXPLORER_BUFF visible.
 * @note Falls back to plain splitScreenUnify() behavior (keep pane 0) if
 * no pane holds a non-explorer buffer — shouldn't normally happen, but a
 * layout with two EXPLORER_BUFF panes is at least handled gracefully.
 */
void splitScreenCloseSidebar(void) {
    if (E.layout.num_panes < 2) {
        splitScreenUnify();
        return;
    }

    for (int i = 0; i < E.layout.num_panes; i++) {
        struct buffer_t *b = bufferFindById(E.layout.panes[i].buffer_id);
        if (!b || b->type != EXPLORER_BUFF) {
            E.layout.panes[0].buffer_id = E.layout.panes[i].buffer_id;
            break;
        }
    }

    splitScreenUnify();
    E.prefix_state = 0;
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
    Buffer *active_buffer = bufferFindById(E.layout.panes[E.layout.active_pane].buffer_id);
    if (active_buffer && active_buffer->type == TERMINAL_BUFF) {
        struct prefix_t prefix = find_prefix("term");
        if (prefix.prefix_id != -1) {
            E.prefix_state = prefix.prefix_id;
        }
    } else if (active_buffer && active_buffer->type == EXPLORER_BUFF) {
        struct prefix_t prefix = find_prefix(bufferFocusPrefix(active_buffer));
        if (prefix.prefix_id != -1) {
            E.prefix_state = prefix.prefix_id;
        }
    } else {
        // Plain FILE_BUFF (or no buffer) — always land in no-prefix, so a
        // "term"/"explorer" prefix left over from the pane you just came
        // from doesn't silently swallow ENTER/arrows/backspace here (they're
        // only mapped under "no-prefix", so a stale prefix state means none
        // of them match anything and every keystroke is just dropped).
        E.prefix_state = 0;
    }

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
int splitScreenSetPaneBuffer(int buffer_id) {
    if (bufferFindById(buffer_id) == NULL) {
        editorSetStatusMessage("Error: invalid buffer ID");
        return -1;
    }

    EditorPane *active = &E.layout.panes[E.layout.active_pane];
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
        free(layout->panes);
        layout->panes = NULL;
    }
}

void freePane(EditorPane *pane)
{
    free(pane);
}
