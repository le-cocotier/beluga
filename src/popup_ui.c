/**
 * @file popup_ui.c
 * @brief See popup_ui.h.
 */

#include "../include/popup_ui.h"
#include "../include/append_buffer.h"
#include "../include/terminal.h"

#include <stdio.h>
#include <string.h>

extern EditorConfig E;

// ─── small private ANSI helpers, same shape every popup module used to
// keep a copy of for itself ────────────────────────────────────────────────

static void ab_move(struct abuf *ab, int row, int col) {
    char esc[32];
    int n = snprintf(esc, sizeof(esc), "\x1b[%d;%dH", row, col);
    abAppend(ab, esc, n);
}

static void ab_sgr(struct abuf *ab, const char *codes) {
    char esc[48];
    int n = snprintf(esc, sizeof(esc), "\x1b[%sm", codes);
    if (n >= (int) sizeof(esc)) n = (int) sizeof(esc) - 1; // defensive: never over-read on truncation
    abAppend(ab, esc, n);
}

#define AB_RESET(ab) ab_sgr(ab, "0")

// ─── theme color plumbing ───────────────────────────────────────────────────

/**
 * @brief Pulls (R, G, B) back out of a theme color string.
 * @details Every E.theme.* field is always exactly "\x1b[38;2;R;G;Bm" (fg)
 * or "\x1b[48;2;R;G;Bm" (bg) — both editorSetThemeColor() and initEditor()'s
 * hardcoded fallback build them with the same ANSI_FG_RGB/ANSI_BG_RGB
 * macros, so this format is guaranteed rather than defensively assumed.
 */
static void parseThemeRgb(const char *ansi, int *r, int *g, int *b) {
    *r = *g = *b = 0;
    if (!ansi) return;
    const char *p = strchr(ansi, '[');
    if (!p) return;
    int kind, mode;
    sscanf(p + 1, "%d;%d;%d;%d;%d", &kind, &mode, r, g, b);
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/** @brief Linear blend of `a` toward `b` by `t` percent (0-100). */
static int blend(int a, int b, int t) { return clamp255(a + (b - a) * t / 100); }

void popupStyleInit(PopupStyle *style) {
    int bg_r, bg_g, bg_b, ac_r, ac_g, ac_b, fg_r, fg_g, fg_b;
    parseThemeRgb(E.theme.BACKGROUND_COLOR, &bg_r, &bg_g, &bg_b);
    // The accent is the theme's keyword color — every theme picks one on
    // purpose as its most eye-catching hue, which is exactly what a border/
    // selection accent wants, without needing a dedicated theme role just
    // for popup chrome.
    parseThemeRgb(E.theme.COLOR_KEYWORD, &ac_r, &ac_g, &ac_b);
    parseThemeRgb(E.theme.COLOR_DEFAULT, &fg_r, &fg_g, &fg_b);

    snprintf(style->chrome, sizeof(style->chrome), "0;48;2;%d;%d;%d;38;2;%d;%d;%d",
            bg_r, bg_g, bg_b, ac_r, ac_g, ac_b);
    snprintf(style->title, sizeof(style->title), "1;48;2;%d;%d;%d;38;2;%d;%d;%d",
            bg_r, bg_g, bg_b, ac_r, ac_g, ac_b);
    snprintf(style->row, sizeof(style->row), "0;48;2;%d;%d;%d;38;2;%d;%d;%d",
            bg_r, bg_g, bg_b, fg_r, fg_g, fg_b);
    snprintf(style->accent_fg, sizeof(style->accent_fg), "38;2;%d;%d;%d", ac_r, ac_g, ac_b);

    // Selected row: background nudged 30% toward the accent (a tint, not a
    // full accent fill — legible without turning into a flat color block),
    // text pushed toward white for contrast against that tint.
    int sel_r = blend(bg_r, ac_r, 30), sel_g = blend(bg_g, ac_g, 30), sel_b = blend(bg_b, ac_b, 30);
    int selfg_r = blend(fg_r, 255, 60), selfg_g = blend(fg_g, 255, 60), selfg_b = blend(fg_b, 255, 60);
    snprintf(style->row_sel, sizeof(style->row_sel), "1;48;2;%d;%d;%d;38;2;%d;%d;%d",
            sel_r, sel_g, sel_b, selfg_r, selfg_g, selfg_b);

    // Shadow: background darkened toward black — a flat dark patch is all
    // a terminal can offer in place of a real soft/alpha-blended shadow.
    int sh_r = blend(bg_r, 0, 55), sh_g = blend(bg_g, 0, 55), sh_b = blend(bg_b, 0, 55);
    snprintf(style->shadow, sizeof(style->shadow), "0;48;2;%d;%d;%d", sh_r, sh_g, sh_b);
}

void popupCenter(int box_w, int box_h, int *origin_x, int *origin_y) {
    *origin_x = (E.screencols - box_w) / 2;
    *origin_y = (E.screenrows - box_h) / 2;
    if (*origin_x < 1) *origin_x = 1;
    if (*origin_y < 1) *origin_y = 1;
}

// ─── chrome ─────────────────────────────────────────────────────────────────

void popupDrawTop(struct abuf *ab, const PopupStyle *style,
                  int origin_x, int row, int box_w) {
    ab_move(ab, row, origin_x);
    ab_sgr(ab, style->chrome);
    abAppend(ab, "╭", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "╮", 3);
    AB_RESET(ab);
}

void popupDrawBottom(struct abuf *ab, const PopupStyle *style,
                     int origin_x, int row, int box_w) {
    ab_move(ab, row, origin_x);
    ab_sgr(ab, style->chrome);
    abAppend(ab, "╰", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "╯", 3);
    AB_RESET(ab);
}

void popupDrawSeparator(struct abuf *ab, const PopupStyle *style,
                        int origin_x, int row, int box_w) {
    ab_move(ab, row, origin_x);
    ab_sgr(ab, style->chrome);
    abAppend(ab, "├", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┤", 3);
    AB_RESET(ab);
}

void popupDrawTitleRow(struct abuf *ab, const PopupStyle *style,
                       int origin_x, int row, int box_w,
                       const char *title, int content_w) {
    ab_move(ab, row, origin_x);
    ab_sgr(ab, style->chrome);
    abAppend(ab, "│ ", 2);
    ab_sgr(ab, style->title);
    char padded[128];
    int n = snprintf(padded, sizeof(padded), "%-*.*s", content_w, content_w, title);
    if (n > content_w) n = content_w;
    abAppend(ab, padded, n);
    ab_sgr(ab, style->chrome);
    abAppend(ab, " │", 2);
    AB_RESET(ab);
}

void popupRowBegin(struct abuf *ab, const PopupStyle *style, int origin_x,
                   int row, int is_selected) {
    ab_move(ab, row, origin_x);
    ab_sgr(ab, style->chrome);
    abAppend(ab, "│", 3);
    popupRowStyle(ab, style, is_selected);
    if (is_selected) {
        // A bare fg-only SGR param changes just the foreground, leaving
        // row_sel's tinted background (already active) untouched — the
        // marker reads as an accent flag sitting on the highlighted row,
        // not a separate box. Re-apply the full row style right after so
        // the rest of the row isn't left in the marker's fg color.
        ab_sgr(ab, style->accent_fg);
        abAppend(ab, "▎", 3);
        popupRowStyle(ab, style, is_selected);
    } else {
        abAppend(ab, " ", 1);
    }
}

void popupRowStyle(struct abuf *ab, const PopupStyle *style, int is_selected) {
    ab_sgr(ab, is_selected ? style->row_sel : style->row);
}

void popupRowEnd(struct abuf *ab, const PopupStyle *style) {
    abAppend(ab, " ", 1);
    ab_sgr(ab, style->chrome);
    abAppend(ab, "│", 3);
    AB_RESET(ab);
}

void popupDrawShadow(struct abuf *ab, const PopupStyle *style,
                     int origin_x, int origin_y, int box_w, int box_h) {
    // Popups aren't always centered (completion opens next to the cursor,
    // which is often near a screen edge) — skip whatever part of the
    // shadow would land outside the terminal rather than writing a cell
    // past the last column/row, which some terminals wrap or otherwise
    // mishandle instead of just clipping.
    int right_col = origin_x + box_w;
    int bottom_row = origin_y + box_h;
    // +2: status bar and message bar both live below E.screenrows.
    int max_row = E.screenrows + 2;

    ab_sgr(ab, style->shadow);
    if (right_col <= E.screencols) {
        for (int r = 1; r < box_h; r++) {
            int row = origin_y + r;
            if (row > max_row) break;
            ab_move(ab, row, right_col);
            abAppend(ab, " ", 1);
        }
    }
    if (bottom_row <= max_row) {
        int start_col = origin_x + 1;
        int cols = box_w;
        if (start_col + cols - 1 > E.screencols)
            cols = E.screencols - start_col + 1;
        if (cols > 0) {
            ab_move(ab, bottom_row, start_col);
            for (int c = 0; c < cols; c++) abAppend(ab, " ", 1);
        }
    }
    AB_RESET(ab);
}
