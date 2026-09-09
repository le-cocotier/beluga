/**
 * @file split_screen.h
 * @brief Split screen management for displaying multiple buffers
 */

#ifndef SPLIT_SCREEN_H_
#define SPLIT_SCREEN_H_

#include "data.h"

/**
 * @brief Initializes split screen system
 */
void splitScreenInit(void);

// Splits the active pane in two, left/right for SPLIT_VERTICAL or
// top/bottom for SPLIT_HORIZONTAL, leaving one column/row between them for
// the divider. The new pane shows the same buffer and takes focus.
//
// Composes without a limit — each call subdivides whichever pane is
// currently active, so the screen can be split to any depth in either
// direction. This replaced splitScreenVertical()/splitScreenHorizontal(),
// which rebuilt the layout as exactly two panes from two given buffer ids
// and so could never produce a third.
//
// Returns 0, or -1 if the active pane is already too small to divide.
int splitScreenSplitActive(SplitMode direction);

/**
 * @brief Returns to single buffer fullscreen
 */
void splitScreenUnify(void);

/**
 * @brief Splits the screen vertically with a fixed-width left pane and
 * the rest of the screen on the right — used for the file-explorer
 * sidebar (unlike splitScreenVertical, which is always a 50/50 split).
 * @param sidebar_buffer_id Buffer ID for the narrow left pane
 * @param main_buffer_id Buffer ID for the right pane
 * @param sidebar_width Width in columns of the left pane, clamped to
 * [1, screencols - 1]
 * @return 0 on success, -1 on failure
 */
int splitScreenWithSidebar(int sidebar_buffer_id, int main_buffer_id, int sidebar_width);

// Tells every terminal buffer currently shown in a pane how big its pane
// is now. Call after anything that changes pane geometry — a split, a
// unify, a sidebar open/close, a window resize — or the shell keeps
// wrapping its output to whatever size it was spawned with.
void splitScreenSyncTerminalSizes(void);

/**
 * @brief Collapses a sidebar split back to fullscreen, keeping whichever
 * pane ISN'T an EXPLORER_BUFF visible (rather than always keeping pane 0,
 * like splitScreenUnify does).
 */
void splitScreenCloseSidebar(void);

/**
 * @brief Switches active pane (focus moves between splits)
 * @return 0 on success, -1 on failure
 */
int splitScreenSwitchPane(void);

/**
 * @brief Updates the active pane's buffer
 * @param buffer_id New buffer ID for active pane
 * @return 0 on success, -1 on failure
 */
int splitScreenSetPaneBuffer(int buffer_id);

/**
 * @brief Gets current screen layout
 * @return Pointer to current ScreenLayout
 */
ScreenLayout *splitScreenGetLayout(void);

/**
 * @brief Gets active pane
 * @return Pointer to active EditorPane
 */
EditorPane *splitScreenGetActivePane(void);

void freePane(EditorPane *pane);
void freeScreenLayout(ScreenLayout *layout);


#endif
