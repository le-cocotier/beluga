/**
 * @file popup_ui.h
 * @brief Shared visual chrome for floating popups (LSP completion, the
 * generic list picker, the buffer switcher). Centralizes what used to be
 * four copies of the same ab_move/ab_sgr helpers, each hardcoding a plain
 * "\x1b[0;37;40m" style — dim white text on literal ANSI black, regardless
 * of the user's actual theme, so a light theme would show every popup as a
 * black box floating over it.
 * @details Colors are derived from the current theme once per draw call
 * (see popupStyleInit()) rather than hardcoded, so popups always sit on
 * the theme's real background and borrow its accent color, whatever the
 * user has picked. which_key.c intentionally keeps its own bottom-strip
 * rendering — it's a full-width hint bar, not a floating box — but reuses
 * popupStyleInit() so its colors stay consistent with everything else.
 */

#ifndef BELUGA_POPUP_UI_H
#define BELUGA_POPUP_UI_H

#include "data.h"

// Precomputed SGR parameter strings (no leading "\x1b[", no trailing "m" —
// ab_sgr() / abAppendSgr() add those) for one popup's worth of drawing.
// Recomputed on every draw call: cheap (a few sscanf calls), and it means a
// live theme change (editor-set-theme-color) is reflected on the very next
// frame instead of needing a cache-invalidation path.
typedef struct
{
    char chrome[40];    // border/corner lines: theme bg + accent fg
    char title[40];     // title row: theme bg + accent fg, bold
    char row[40];       // normal row: theme bg + default fg
    char row_sel[40];   // selected row: accent-tinted bg + bright fg, bold
    char accent_fg[16]; // bare accent fg only, no bg — for the selection
                        // marker, layered on top of whatever bg (row or
                        // row_sel) is already active
    char shadow[24];    // drop-shadow cells: darkened bg only
} PopupStyle;

// Reads the live theme (E.theme) and fills in `style`.
void popupStyleInit(PopupStyle *style);

// Centers a box_w x box_h box on screen, clamped to stay on-screen even if
// that means overlapping row/col 1 for an oversized box.
void popupCenter(int box_w, int box_h, int *origin_x, int *origin_y);

// ── Chrome, one screen row at a time — all positions are 1-based absolute
// screen coordinates, matching the rest of the renderer (ab_move in
// output.c/lsp_ui.c etc). Each call fully draws its row, including setting
// the SGR style and resetting after — callers don't need to bracket these.

void popupDrawTop(struct abuf *ab, const PopupStyle *style,
                  int origin_x, int row, int box_w);
void popupDrawBottom(struct abuf *ab, const PopupStyle *style,
                     int origin_x, int row, int box_w);
void popupDrawSeparator(struct abuf *ab, const PopupStyle *style,
                        int origin_x, int row, int box_w);
// content_w excludes the border/margin (box_w - 4); title is left-padded
// to fill it.
void popupDrawTitleRow(struct abuf *ab, const PopupStyle *style,
                       int origin_x, int row, int box_w,
                       const char *title, int content_w);

// Starts one content row: moves to (origin_x, row), sets the row's base
// style (row_sel if is_selected, row otherwise) and prints the left
// border plus a one-column selection marker ("▎" in the accent color when
// selected, a blank column otherwise). The row's base SGR is left active,
// so the caller can abAppend() its label directly, or briefly switch to
// another fg color (e.g. a completion "kind" tag) and switch back with
// popupRowStyle() before continuing.
void popupRowBegin(struct abuf *ab, const PopupStyle *style, int origin_x,
                   int row, int is_selected);
// Re-applies the row's own base style — for switching back after a
// mid-row color change (e.g. after a colored kind tag).
void popupRowStyle(struct abuf *ab, const PopupStyle *style, int is_selected);
// Closes one content row: one space of right padding, the right border
// (in the chrome/accent color, matching the vertical frame's other side),
// and a reset.
void popupRowEnd(struct abuf *ab, const PopupStyle *style);

// One-cell-wide column of shadow down the right edge and one-row strip
// along the bottom, offset down-right by a column/row so it reads as a
// shadow cast by the box rather than a second border. Call last, after
// the bottom border.
void popupDrawShadow(struct abuf *ab, const PopupStyle *style,
                     int origin_x, int origin_y, int box_w, int box_h);

#endif // BELUGA_POPUP_UI_H
