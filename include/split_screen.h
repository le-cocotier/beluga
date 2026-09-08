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

/**
 * @brief Splits screen vertically (left-right)
 * @param buffer_id_left Buffer ID for left pane
 * @param buffer_id_right Buffer ID for right pane
 * @return 0 on success, -1 on failure
 */
int splitScreenVertical(int buffer_id_left, int buffer_id_right);

/**
 * @brief Splits screen horizontally (top-bottom)
 * @param buffer_id_top Buffer ID for top pane
 * @param buffer_id_bottom Buffer ID for bottom pane
 * @return 0 on success, -1 on failure
 */
int splitScreenHorizontal(int buffer_id_top, int buffer_id_bottom);

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
