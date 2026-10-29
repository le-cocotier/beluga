/*
* @file src/ansi_parser.c
* @author Giorgio
* @brief A terminal pane's screen: the output of the program on its
* pseudo-terminal, parsed into the buffer's rows.
* @details The rows below screen_top_row are the screen; the ones above are
* scrollback. Each Row holds one Utf8Char per character with an attrs[]
* entry beside it, and the parser's cursor counts screen columns: a wide
* character (CJK, emoji) covers two, a combining mark none. Full-screen
* programs (vim, less, htop) get scroll regions, the alternate screen,
* insert/delete line and 256-colour/truecolour SGR.
*/

#include "../include/ansi_parser.h"
#include "../include/galloc.h"
#include "../include/buffer.h"
#include "../include/utf8.h"
#include <string.h>

// The pane's width isn't known here; cursor columns are clamped to this
// instead, so a stray "move right 100000" can't make huge rows.
#define TERM_MAX_COLUMNS 1024
#define TERM_MAX_PARAM 100000

/***********************************************************************************************************************
 *
 *                                                 Screen geometry
 *
 **********************************************************************************************************************/

static int screenHeight(const TermParserState* ts)
{
    return ts->pane_height > 0 ? ts->pane_height : 24; // fallback if never set
}

// The scroll region's first and last rows, as buffer row indexes.
static int regionTop(const TermParserState* ts)
{
    int top = ts->scroll_top;
    if (top < 0 || top >= screenHeight(ts)) top = 0;
    return ts->screen_top_row + top;
}

static int regionBottom(const TermParserState* ts)
{
    int bottom = ts->scroll_bottom - 1;
    if (ts->scroll_bottom <= 0 || bottom >= screenHeight(ts)) bottom = screenHeight(ts) - 1;
    return ts->screen_top_row + bottom;
}

// The last column the cursor can reach: the pane's, once it's known.
static int lastColumn(const TermParserState* ts)
{
    return ts->pane_width > 0 && ts->pane_width <= TERM_MAX_COLUMNS ? ts->pane_width - 1
                                                                    : TERM_MAX_COLUMNS - 1;
}

static int regionIsWholeScreen(const TermParserState* ts)
{
    return regionTop(ts) == ts->screen_top_row &&
           regionBottom(ts) == ts->screen_top_row + screenHeight(ts) - 1;
}

/***********************************************************************************************************************
 *
 *                                                 Cells of a row
 *
 **********************************************************************************************************************/

/**
 * @brief Ensure row->attrs is allocated with room for `size` cells plus one.
 * Lazy allocation: file buffers never call this, so their rows stay NULL.
 * A row that had none yet gets default attributes for what it holds.
 * @return 0 when out of memory
 */
static int ensureAttrs(Row* row, const int size)
{
    const int had = row->attrs != NULL;
    TermCellAttr* new_attrs = grealloc(row->attrs, (size_t) (size + 1) * sizeof(TermCellAttr));
    if (!new_attrs) return 0;
    if (!had) memset(new_attrs, 0, (size_t) (size + 1) * sizeof(TermCellAttr));
    row->attrs = new_attrs;
    return 1;
}

// Inserts one character with its attribute at index `at` (0..size).
static void cellInsert(Row* row, const int at, const Utf8Char* c, const TermCellAttr attr)
{
    Utf8Char* chars;
    if (at < 0 || at > row->size) return;
    chars = grealloc(row->characters, (size_t) (row->size + 2) * sizeof(Utf8Char));
    if (!chars) return;
    row->characters = chars;
    if (!ensureAttrs(row, row->size + 1)) return;
    memmove(&row->characters[at + 1], &row->characters[at], (size_t) (row->size - at) * sizeof(Utf8Char));
    memmove(&row->attrs[at + 1], &row->attrs[at], (size_t) (row->size - at) * sizeof(TermCellAttr));
    row->characters[at] = *c;
    row->attrs[at] = attr;
    row->size++;
    END_OF_STRING(row->characters[row->size]);
}

// Removes the characters [from, to).
static void cellDelete(Row* row, const int from, int to)
{
    if (to > row->size) to = row->size;
    if (from < 0 || from >= to) return;
    memmove(&row->characters[from], &row->characters[to], (size_t) (row->size - to) * sizeof(Utf8Char));
    if (row->attrs)
        memmove(&row->attrs[from], &row->attrs[to], (size_t) (row->size - to) * sizeof(TermCellAttr));
    row->size -= to - from;
    END_OF_STRING(row->characters[row->size]);
}

static void rowTruncate(Row* row, const int at)
{
    if (at < row->size)
        cellDelete(row, at, row->size);
}

/**
 * @brief The index of the character at screen column `col`.
 * @param start_col set to the column that character starts at: below `col`
 * when `col` falls on the second half of a wide character, the row's width
 * when `col` is past its end (the index is then row->size).
 * @details Combining marks belong to the character before them, so the
 * index returned is never one of them.
 */
static int indexAtColumn(const Row* row, const int col, int* start_col)
{
    int i = 0;
    int c = 0;
    int w;
    while (i < row->size)
    {
        w = utf8CharWidth(&row->characters[i]);
        if (w == 0) { i++; continue; }
        if (c >= col || c + w > col) break;
        c += w;
        i++;
    }
    *start_col = c;
    return i;
}

static int rowWidth(const Row* row)
{
    int width;
    indexAtColumn(row, TERM_MAX_COLUMNS * 4, &width);
    return width;
}

// A blank cell as an erase leaves it: the current background (xterm's
// "background colour erase"), nothing else.
static TermCellAttr eraseAttr(const TermParserState* ts)
{
    TermCellAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.bg = ts->current_attr.bg;
    return attr;
}

static void insertBlanks(Row* row, const int at, const int n, const TermCellAttr attr)
{
    Utf8Char space;
    int i;
    space.character[0] = ' ';
    space.length = 1;
    for (i = 0; i < n; i++)
        cellInsert(row, at, &space, attr);
}

/**
 * @brief Makes column `col` the start of a character and returns its index.
 * @details Past the row's end, blanks are added up to it (as a terminal's
 * screen already has them); in the middle of a wide character, that
 * character becomes two blanks, as on a terminal when half of one is
 * overwritten.
 */
static int splitAt(Row* row, const int col)
{
    int start;
    int i = indexAtColumn(row, col, &start);
    int w;
    int end;
    TermCellAttr attr;
    TermCellAttr blank;
    if (start == col)
        return i;
    memset(&blank, 0, sizeof(blank));
    if (i == row->size)
    {
        insertBlanks(row, row->size, col - start, blank);
        return row->size;
    }
    w = utf8CharWidth(&row->characters[i]);
    attr = row->attrs ? row->attrs[i] : blank;
    for (end = i + 1; end < row->size && utf8CharWidth(&row->characters[end]) == 0; end++)
        ;
    cellDelete(row, i, end);
    insertBlanks(row, i, w, attr);
    return i + (col - start);
}

/***********************************************************************************************************************
 *
 *                                                 Rows of the screen
 *
 **********************************************************************************************************************/

// Makes the rows up to index `last` exist.
static void ensureRows(struct buffer_t* buf, const int last)
{
    Utf8Char end_string;
    int before;
    END_OF_STRING(end_string);
    while (buf->numrows <= last)
    {
        before = buf->numrows;
        bufferInsertRow(buf, buf->numrows, &end_string, 0);
        if (buf->numrows == before) return; // out of memory
    }
}

/**
 * @brief Get (creating if needed) the Row this buffer's parser cursor is on.
 */
static Row* currentRow(struct buffer_t* buf)
{
    ensureRows(buf, buf->term_state.cursor_y);
    if (buf->term_state.cursor_y >= buf->numrows)
        return NULL;
    return &buf->row[buf->term_state.cursor_y];
}

static void clearRow(Row* row)
{
    row->size = 0;
    END_OF_STRING(row->characters[0]);
}

static void removeRow(struct buffer_t* buf, const int at)
{
    if (at < 0 || at >= buf->numrows) return;
    bufferFreeRow(&buf->row[at]);
    memmove(&buf->row[at], &buf->row[at + 1], (size_t) (buf->numrows - at - 1) * sizeof(Row));
    buf->numrows--;
}

static void insertBlankRow(struct buffer_t* buf, const int at)
{
    Utf8Char end_string;
    END_OF_STRING(end_string);
    bufferInsertRow(buf, at, &end_string, 0);
}

/**
 * @brief Scrolls rows [top, bottom] up by n: the top ones go, blank ones
 * come in at the bottom. Rows outside that range don't move.
 */
static void scrollUp(struct buffer_t* buf, const int top, const int bottom, int n)
{
    if (top > bottom) return;
    if (n > bottom - top + 1) n = bottom - top + 1;
    ensureRows(buf, bottom);
    if (bottom >= buf->numrows) return;
    while (n-- > 0)
    {
        removeRow(buf, top);
        insertBlankRow(buf, bottom);
    }
}

// The reverse: blank rows come in at the top, the bottom ones go.
static void scrollDown(struct buffer_t* buf, const int top, const int bottom, int n)
{
    if (top > bottom) return;
    if (n > bottom - top + 1) n = bottom - top + 1;
    ensureRows(buf, bottom);
    if (bottom >= buf->numrows) return;
    while (n-- > 0)
    {
        removeRow(buf, bottom);
        insertBlankRow(buf, top);
    }
}

/***********************************************************************************************************************
 *
 *                                                 Cursor movement and text
 *
 **********************************************************************************************************************/

static void lineFeed(struct buffer_t* buf);

/**
 * @brief Write one decoded Utf8Char at the cursor, with the current SGR
 * attribute, then move the cursor past it (one or two columns; a combining
 * mark joins the character before it and doesn't move it).
 * @details Lines wrap at the pane's width the way xterm's do: a character
 * written in the last column leaves the cursor there with a wrap pending,
 * and only the next character goes to the start of the next line (so a
 * program can fill the last column, then move or CR LF, without an empty
 * line in between). A wide character that doesn't fit wraps whole.
 * Finding the character at a column walks the row, so a run of text
 * remembers where it is (col_cache_*): each character of `cat`'s output
 * doesn't walk its line again. Anything else the parser does forgets it
 * (see ansiParserFeed()).
 */
static void putChar(struct buffer_t* buf, const Utf8Char* c)
{
    TermParserState* ts = &buf->term_state;
    Row* row;
    const int w = utf8CharWidth(c);
    const int last = lastColumn(ts);
    int start;
    int end;
    int col;

    if (w > 0 && (ts->wrap_pending || (w == 2 && ts->cursor_x == last && last > 0)))
    {
        ts->wrap_pending = 0;
        ts->cursor_x = 0;
        lineFeed(buf);
        ts->col_cache_valid = 0;
    }
    row = currentRow(buf);
    if (!row) return;

    if (w == 0)
    {
        // With a wrap pending, the cursor still sits on the character the
        // mark belongs to: it goes after it.
        start = splitAt(row, ts->cursor_x + (ts->wrap_pending ? 1 : 0));
        if (ts->cursor_x > 0 || ts->wrap_pending)
            cellInsert(row, start, c, ts->current_attr);
        ts->col_cache_valid = 0;
        return;
    }

    if (ts->col_cache_valid && ts->col_cache_y == ts->cursor_y &&
        ts->col_cache_x == ts->cursor_x && ts->col_cache_idx <= row->size)
        start = ts->col_cache_idx;
    else
        start = splitAt(row, ts->cursor_x);

    // The characters covering the columns it takes, with their marks.
    col = ts->cursor_x;
    end = start;
    while (end < row->size && col < ts->cursor_x + w)
        col += utf8CharWidth(&row->characters[end++]);
    while (end < row->size && utf8CharWidth(&row->characters[end]) == 0)
        end++;
    // A wide character overwritten by half: its other half goes blank.
    if (col > ts->cursor_x + w)
        end = splitAt(row, ts->cursor_x + w);
    cellDelete(row, start, end);
    cellInsert(row, start++, c, ts->current_attr);

    if (ts->cursor_x + w > last)
    {
        // The last column: the cursor stays on it until the next character.
        ts->wrap_pending = ts->pane_width > 0;
        ts->cursor_x = last;
        ts->col_cache_valid = 0;
        return;
    }
    ts->cursor_x += w;
    ts->col_cache_valid = 1;
    ts->col_cache_y = ts->cursor_y;
    ts->col_cache_x = ts->cursor_x;
    ts->col_cache_idx = start;
}

/**
 * @brief Index (LF, VT, FF, ESC D): down one row. At the bottom of the scroll
 * region the region scrolls up; on the normal screen with no region set, the
 * top row goes into the scrollback instead, as a shell's output does.
 */
static void lineFeed(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    const int height = screenHeight(ts);
    ts->wrap_pending = 0;
    if (!ts->alt_screen && regionIsWholeScreen(ts) && ts->cursor_y >= regionBottom(ts))
    {
        ts->cursor_y++;
        if (ts->cursor_y >= ts->screen_top_row + height)
            ts->screen_top_row = ts->cursor_y - height + 1;
        return;
    }
    if (ts->cursor_y == regionBottom(ts))
        scrollUp(buf, regionTop(ts), regionBottom(ts), 1);
    else if (ts->cursor_y < ts->screen_top_row + height - 1)
        ts->cursor_y++;
}

// Reverse index (ESC M): up one row, scrolling the region down at its top.
static void reverseIndex(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    ts->wrap_pending = 0;
    if (ts->cursor_y == regionTop(ts))
        scrollDown(buf, regionTop(ts), regionBottom(ts), 1);
    else if (ts->cursor_y > ts->screen_top_row)
        ts->cursor_y--;
}

static void carriageReturn(struct buffer_t* buf)
{
    buf->term_state.cursor_x = 0;
    buf->term_state.wrap_pending = 0;
}

static void backspace(struct buffer_t* buf)
{
    // With a wrap pending the cursor is still on the last column: BS goes
    // to the one before it, as on xterm.
    buf->term_state.wrap_pending = 0;
    if (buf->term_state.cursor_x > 0)
        buf->term_state.cursor_x--;
}

/**
 * @brief HT: move to the next 8-column tab stop. Like a real terminal it
 * only moves the cursor: text it passes over stays, and a stop past the
 * row's end is padded with blanks by the next putChar().
 */
static void horizontalTab(struct buffer_t* buf)
{
    buf->term_state.wrap_pending = 0;
    buf->term_state.cursor_x = (buf->term_state.cursor_x / 8 + 1) * 8;
    if (buf->term_state.cursor_x > lastColumn(&buf->term_state))
        buf->term_state.cursor_x = lastColumn(&buf->term_state);
}

// After a cursor move (which also drops a pending wrap): back on screen.
static void clampCursorToScreen(TermParserState* ts)
{
    const int bottom = ts->screen_top_row + screenHeight(ts) - 1;
    ts->wrap_pending = 0;
    if (ts->cursor_y < ts->screen_top_row) ts->cursor_y = ts->screen_top_row;
    if (ts->cursor_y > bottom) ts->cursor_y = bottom;
    if (ts->cursor_x < 0) ts->cursor_x = 0;
    if (ts->cursor_x > lastColumn(ts)) ts->cursor_x = lastColumn(ts);
}

static void saveCursor(TermParserState* ts)
{
    ts->saved_cursor_x = ts->cursor_x;
    ts->saved_cursor_y = ts->cursor_y - ts->screen_top_row; // screen-relative
    ts->saved_attr = ts->current_attr;
}

static void restoreCursor(TermParserState* ts)
{
    ts->cursor_x = ts->saved_cursor_x;
    ts->cursor_y = ts->screen_top_row + ts->saved_cursor_y;
    ts->current_attr = ts->saved_attr;
    clampCursorToScreen(ts);
}

/***********************************************************************************************************************
 *
 *                                                 Erasing
 *
 **********************************************************************************************************************/

// EL 0: from the cursor to the end of the line.
static void eraseToEndOfLine(struct buffer_t* buf, Row* row)
{
    const int col = buf->term_state.cursor_x;
    if (col >= rowWidth(row)) return;
    rowTruncate(row, splitAt(row, col));
}

// Blanks columns [from, to) of the row, as far as it reaches.
static void blankColumns(struct buffer_t* buf, Row* row, const int from, int to)
{
    const int width = rowWidth(row);
    int start;
    int end;
    if (to > width) to = width;
    if (from >= to) return;
    start = splitAt(row, from);
    end = to == width ? row->size : splitAt(row, to);
    cellDelete(row, start, end);
    insertBlanks(row, start, to - from, eraseAttr(&buf->term_state));
}

static void eraseInLine(struct buffer_t* buf, const int mode)
{
    Row* row = currentRow(buf);
    if (!row) return;
    if (mode == 0)
        eraseToEndOfLine(buf, row);
    else if (mode == 1) // start of line through the cursor, inclusive
        blankColumns(buf, row, 0, buf->term_state.cursor_x + 1);
    else if (mode == 2)
        clearRow(row);
}

static void eraseInDisplay(struct buffer_t* buf, const int mode)
{
    TermParserState* ts = &buf->term_state;
    const int screen_bottom = ts->screen_top_row + screenHeight(ts) - 1;
    int y;
    if (mode == 0)
    {
        // From the cursor to the end of the screen — the cursor doesn't move.
        eraseInLine(buf, 0);
        for (y = ts->cursor_y + 1; y <= screen_bottom && y < buf->numrows; y++)
            clearRow(&buf->row[y]);
    }
    else if (mode == 1)
    {
        for (y = ts->screen_top_row; y < ts->cursor_y && y < buf->numrows; y++)
            clearRow(&buf->row[y]);
        eraseInLine(buf, 1);
    }
    else if (mode == 2)
    {
        for (y = ts->screen_top_row; y <= screen_bottom && y < buf->numrows; y++)
            clearRow(&buf->row[y]);
    }
}

/***********************************************************************************************************************
 *
 *                                                 Alternate screen
 *
 **********************************************************************************************************************/

static void freeRows(Row* rows, const int count)
{
    int i;
    for (i = 0; i < count; i++)
        bufferFreeRow(&rows[i]);
    gfree(rows);
}

/**
 * @brief Switches to the alternate screen: a blank screen with no
 * scrollback, which the program draws on until it switches back. The normal
 * screen (and its scrollback) is kept aside meanwhile.
 */
static void enterAltScreen(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    if (ts->alt_screen) return;
    ts->main_rows = buf->row;
    ts->main_numrows = buf->numrows;
    ts->main_top_row = ts->screen_top_row;
    buf->row = NULL;
    buf->numrows = 0;
    ts->cursor_y -= ts->screen_top_row; // same place on the screen
    ts->screen_top_row = 0;
    ts->alt_screen = 1;
    ensureRows(buf, screenHeight(ts) - 1);
}

static void leaveAltScreen(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    if (!ts->alt_screen) return;
    freeRows(buf->row, buf->numrows);
    buf->row = ts->main_rows;
    buf->numrows = ts->main_numrows;
    ts->cursor_y += ts->main_top_row - ts->screen_top_row;
    ts->screen_top_row = ts->main_top_row;
    ts->main_rows = NULL;
    ts->main_numrows = 0;
    ts->alt_screen = 0;
    clampCursorToScreen(ts);
}

void ansiParserRelease(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    if (!ts->alt_screen) return;
    freeRows(ts->main_rows, ts->main_numrows);
    ts->main_rows = NULL;
    ts->main_numrows = 0;
    ts->alt_screen = 0;
}

int ansiParserCursorIndex(const struct buffer_t* buf)
{
    const TermParserState* ts = &buf->term_state;
    const Row* row;
    int start;
    int i;
    if (ts->cursor_y < 0 || ts->cursor_y >= buf->numrows)
        return ts->cursor_x;
    row = &buf->row[ts->cursor_y];
    i = indexAtColumn(row, ts->cursor_x, &start);
    // Past the end, a column is a blank character on screen.
    return i == row->size ? i + (ts->cursor_x - start) : i;
}

/***********************************************************************************************************************
 *
 *                                                 Control sequences
 *
 **********************************************************************************************************************/

/**
 * @brief Apply the SGR (Select Graphic Rendition) parameters from index *i,
 * advancing *i past the ones a colour (38;5;n, 38;2;r;g;b) consumed.
 */
static void applySgr(TermParserState* ts, int* i)
{
    const int p = ts->params[*i];
    unsigned int color;
    int r;
    int g;
    int b;
    if (p == 38 || p == 48 || p == 58)
    {
        color = 0;
        if (*i + 2 < ts->nparams && ts->params[*i + 1] == 5)
        {
            color = (unsigned int) (ts->params[*i + 2] & 0xFF) + 1;
            *i += 2;
        }
        else if (*i + 4 < ts->nparams && ts->params[*i + 1] == 2)
        {
            r = ts->params[*i + 2] & 0xFF;
            g = ts->params[*i + 3] & 0xFF;
            b = ts->params[*i + 4] & 0xFF;
            color = TERM_COLOR_RGB | (unsigned int) (r << 16 | g << 8 | b);
            *i += 4;
        }
        else
        {
            *i = ts->nparams; // malformed: the rest can't be read reliably
            return;
        }
        if (p == 38) ts->current_attr.fg = color;
        else if (p == 48) ts->current_attr.bg = color;
        // 58 (underline colour) isn't shown: its parameters are just skipped
        return;
    }
    if (p == 0)
        memset(&ts->current_attr, 0, sizeof(TermCellAttr));
    else if (p == 1) ts->current_attr.bold = 1;
    else if (p == 4) ts->current_attr.underline = 1;
    else if (p == 7) ts->current_attr.reverse = 1;
    else if (p == 22) ts->current_attr.bold = 0;
    else if (p == 24) ts->current_attr.underline = 0;
    else if (p == 27) ts->current_attr.reverse = 0;
    else if (p >= 30 && p <= 37) ts->current_attr.fg = (unsigned int) (p - 30) + 1;
    else if (p == 39) ts->current_attr.fg = 0;
    else if (p >= 40 && p <= 47) ts->current_attr.bg = (unsigned int) (p - 40) + 1;
    else if (p == 49) ts->current_attr.bg = 0;
    else if (p >= 90 && p <= 97) ts->current_attr.fg = (unsigned int) (p - 90) + 9;
    else if (p >= 100 && p <= 107) ts->current_attr.bg = (unsigned int) (p - 100) + 9;
}

/**
 * @brief Private modes (CSI ? n h / l): the alternate screen. The others
 * (cursor visibility, bracketed paste, mouse, ...) don't change the text.
 */
static void dispatchPrivateMode(struct buffer_t* buf, const int set)
{
    TermParserState* ts = &buf->term_state;
    int i;
    ts->wrap_pending = 0;
    for (i = 0; i < ts->nparams; i++)
    {
        switch (ts->params[i])
        {
        case 1049: // save the cursor, then the alternate screen, cleared
            if (set)
            {
                saveCursor(ts);
                enterAltScreen(buf);
                eraseInDisplay(buf, 2);
            }
            else
            {
                leaveAltScreen(buf);
                restoreCursor(ts);
            }
            break;
        case 1047:
        case 47:
            if (set) enterAltScreen(buf);
            else leaveAltScreen(buf);
            break;
        case 1048:
            if (set) saveCursor(ts);
            else restoreCursor(ts);
            break;
        default:
            break;
        }
    }
}

/**
 * @brief Dispatch a fully-parsed CSI sequence (params + final byte) to the
 * appropriate terminal action.
 */
static void dispatchCsi(struct buffer_t* buf, char final)
{
    TermParserState* ts = &buf->term_state;
    const int p0 = ts->nparams > 0 ? ts->params[0] : 0;
    const int n = p0 ? p0 : 1; // the usual count: 0 or missing means 1
    const int height = screenHeight(ts);
    int row;
    int col;
    int top;
    int bottom;
    int i;
    Row* line;

    // Everything but colours moves or changes the screen under the
    // cursor: a pending wrap doesn't survive it.
    if (final != 'm')
        ts->wrap_pending = 0;

    switch (final)
    {
    case 'A': // Cursor Up — relative to screen, clamp at screen top, not absolute 0
        ts->cursor_y -= n;
        clampCursorToScreen(ts);
        break;
    case 'B': // Cursor Down — clamped at the screen's bottom row, like a
        // terminal; unclamped, a large count made rows up to it.
        ts->cursor_y += n;
        clampCursorToScreen(ts);
        break;
    case 'C':
        ts->cursor_x += n;
        clampCursorToScreen(ts);
        break;
    case 'D':
        ts->cursor_x -= n;
        clampCursorToScreen(ts);
        break;
    case 'E': // Cursor Next Line
        ts->cursor_y += n;
        ts->cursor_x = 0;
        clampCursorToScreen(ts);
        break;
    case 'F': // Cursor Previous Line
        ts->cursor_y -= n;
        ts->cursor_x = 0;
        clampCursorToScreen(ts);
        break;

    case 'H':
    case 'f': // Cursor Position — row/col are screen-relative, 1-indexed
        // Clamped to the screen, as a terminal does: "\x1b[99999;1H"
        // must not make 99999 rows.
        row = ts->nparams > 0 && ts->params[0] > 0 ? ts->params[0] : 1;
        col = ts->nparams > 1 && ts->params[1] > 0 ? ts->params[1] : 1;
        ts->cursor_y = ts->screen_top_row + row - 1;
        ts->cursor_x = col - 1;
        clampCursorToScreen(ts);
        break;
    case 'G': // Cursor Horizontal Absolute — 1-indexed column
    case '`':
        ts->cursor_x = n - 1;
        clampCursorToScreen(ts);
        break;
    case 'd': // Line Position Absolute — 1-indexed, screen-relative
        ts->cursor_y = ts->screen_top_row + n - 1;
        clampCursorToScreen(ts);
        break;

    case 'J': // Erase in display
        eraseInDisplay(buf, p0);
        break;
    case 'K': // Erase in line
        eraseInLine(buf, p0);
        break;
    case 'X': // Erase Character — blank n cells from the cursor, cursor stays
        line = currentRow(buf);
        if (line)
            blankColumns(buf, line, ts->cursor_x, ts->cursor_x + n);
        break;
    case 'P': // Delete Character — shift the rest of the line left by n
        line = currentRow(buf);
        if (line && ts->cursor_x < rowWidth(line))
            cellDelete(line, splitAt(line, ts->cursor_x),
                       ts->cursor_x + n >= rowWidth(line) ? line->size
                                                          : splitAt(line, ts->cursor_x + n));
        break;
    case '@': // Insert Character — open n blanks at the cursor
        line = currentRow(buf);
        if (line && ts->cursor_x < rowWidth(line)) // nothing to push right otherwise
            insertBlanks(line, splitAt(line, ts->cursor_x),
                         n < TERM_MAX_COLUMNS ? n : TERM_MAX_COLUMNS, eraseAttr(ts));
        break;

    case 'L': // Insert Line: blank rows at the cursor, the region's last ones go
        if (ts->cursor_y >= regionTop(ts) && ts->cursor_y <= regionBottom(ts))
        {
            scrollDown(buf, ts->cursor_y, regionBottom(ts), n);
            ts->cursor_x = 0;
        }
        break;
    case 'M': // Delete Line: the rows below move up, blank ones come in
        if (ts->cursor_y >= regionTop(ts) && ts->cursor_y <= regionBottom(ts))
        {
            scrollUp(buf, ts->cursor_y, regionBottom(ts), n);
            ts->cursor_x = 0;
        }
        break;
    case 'S': // Scroll Up, within the region
        scrollUp(buf, regionTop(ts), regionBottom(ts), n);
        break;
    case 'T': // Scroll Down
        scrollDown(buf, regionTop(ts), regionBottom(ts), n);
        break;
    case 'r': // Set the scroll region (DECSTBM), 1-indexed; then home
        top = ts->nparams > 0 && ts->params[0] > 0 ? ts->params[0] : 1;
        bottom = ts->nparams > 1 && ts->params[1] > 0 ? ts->params[1] : height;
        if (bottom > height) bottom = height;
        if (top < bottom)
        {
            ts->scroll_top = top - 1;
            ts->scroll_bottom = bottom == height ? 0 : bottom;
            ts->cursor_x = 0;
            ts->cursor_y = ts->screen_top_row;
        }
        break;

    case 'm': // SGR
        if (ts->nparams == 0)
            memset(&ts->current_attr, 0, sizeof(TermCellAttr));
        else
            for (i = 0; i < ts->nparams; i++)
                applySgr(ts, &i);
        break;

    case 's':
        saveCursor(ts);
        break;
    case 'u':
        restoreCursor(ts);
        break;

    default:
        break; // other final bytes (modes, reports, ...) don't change the text
    }
}

// RIS (ESC c): back to the initial state, on the normal screen, cleared.
static void resetTerminal(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    leaveAltScreen(buf);
    memset(&ts->current_attr, 0, sizeof(TermCellAttr));
    ts->scroll_top = 0;
    ts->scroll_bottom = 0;
    eraseInDisplay(buf, 2);
    ts->cursor_x = 0;
    ts->cursor_y = ts->screen_top_row;
}

static void dispatchEsc(struct buffer_t* buf, const unsigned char byte)
{
    TermParserState* ts = &buf->term_state;
    ts->state = TPARSE_GROUND;
    if (byte != '[' && byte != '7')
        ts->wrap_pending = 0;
    switch (byte)
    {
    case '[':
        ts->state = TPARSE_CSI;
        ts->nparams = 0;
        ts->csi_private = 0;
        ts->csi_intermediate = 0;
        memset(ts->params, 0, sizeof(ts->params));
        break;
    // OSC, and the other string sequences (DCS, SOS, PM, APC): their
    // contents are discarded up to BEL or ST.
    case ']':
    case 'P':
    case 'X':
    case '^':
    case '_':
        ts->state = TPARSE_OSC;
        break;
    case '7': saveCursor(ts); break;
    case '8': restoreCursor(ts); break;
    case 'D': lineFeed(buf); break;                           // IND
    case 'E': carriageReturn(buf); lineFeed(buf); break;      // NEL
    case 'M': reverseIndex(buf); break;                       // RI
    case 'c': resetTerminal(buf); break;                      // RIS
    default:
        // ESC ( B, ESC ) 0, ESC # 8, ...: character sets and the like, one
        // more byte to swallow so it doesn't show up as text.
        if (byte >= 0x20 && byte <= 0x2F)
            ts->state = TPARSE_ESC_INTER;
        break; // ESC = / ESC > (keypad modes) and others: nothing to do
    }
}

void ansiParserFeed(struct buffer_t* buf, const unsigned char* data, size_t len)
{
    TermParserState* ts = &buf->term_state;
    size_t i;
    unsigned char byte;
    Utf8Char c;

    ts->col_cache_valid = 0;
    for (i = 0; i < len; i++)
    {
        byte = data[i];
        // Only a run of text keeps putChar()'s place in the row.
        if (ts->state != TPARSE_GROUND || byte < 0x20 || byte == 0x7f)
            ts->col_cache_valid = 0;

        switch (ts->state)
        {
        case TPARSE_GROUND:
            if (byte == 0x1b)
            {
                ts->utf8_pending_len = 0; // an escape cuts a broken character short
                ts->state = TPARSE_ESC;
                break;
            }
            if (byte == '\r')
            {
                carriageReturn(buf);
                break;
            }
            if (byte == '\n' || byte == 0x0b || byte == 0x0c) // VT and FF move down like LF
            {
                lineFeed(buf);
                break;
            }
            if (byte == '\b' || byte == 0x7f)
            {
                backspace(buf);
                break;
            }
            if (byte == '\t')
            {
                horizontalTab(buf);
                break;
            }

            // BEL, SO/SI and the remaining C0 controls have no visible
            // effect in a buffer: consume them so they never reach the text.
            if (byte < 0x20) break;

            // UTF-8 reassembly: figure out expected length from lead byte
            if (ts->utf8_pending_len == 0)
            {
                if (byte < 0x80) ts->utf8_expected_len = 1;
                else if (byte < 0xE0) ts->utf8_expected_len = 2;
                else if (byte < 0xF0) ts->utf8_expected_len = 3;
                else ts->utf8_expected_len = 4;
            }
            ts->utf8_pending[ts->utf8_pending_len++] = byte;

            if (ts->utf8_pending_len == ts->utf8_expected_len)
            {
                memcpy(c.character, ts->utf8_pending, (size_t) ts->utf8_pending_len);
                c.length = (char) ts->utf8_pending_len;
                putChar(buf, &c);
                ts->utf8_pending_len = 0;
            }
            break;

        case TPARSE_ESC:
            dispatchEsc(buf, byte);
            break;

        case TPARSE_ESC_INTER:
            if (byte >= 0x30 && byte <= 0x7E) ts->state = TPARSE_GROUND;
            else if (byte == 0x1b) ts->state = TPARSE_ESC;
            break;

        case TPARSE_CSI:
            if (byte >= '0' && byte <= '9')
            {
                if (ts->nparams == 0) ts->nparams = 1;
                // Capped: no real sequence needs more, and an endless digit
                // run would overflow.
                if (ts->params[ts->nparams - 1] < TERM_MAX_PARAM)
                    ts->params[ts->nparams - 1] = ts->params[ts->nparams - 1] * 10 + (byte - '0');
            }
            else if (byte == ';' || byte == ':') // ':' separates 38:5:n's parts
            {
                if (ts->nparams == 0) ts->nparams = 1; // ";5H": the first one is empty
                if (ts->nparams < CSI_MAX_PARAMS) ts->nparams++;
            }
            else if (byte >= 0x3C && byte <= 0x3F) // '<' '=' '>' '?' prefix
            {
                ts->csi_private = byte;
            }
            else if (byte >= 0x20 && byte <= 0x2F)
            {
                ts->csi_intermediate = 1;
            }
            else if (byte >= 0x40 && byte <= 0x7E) // final byte
            {
                // Private sequences (?25l cursor visibility, >c device
                // attributes, ...) share final bytes with the standard
                // ones: only the alternate screen's are acted on, and none
                // is misread as an erase or a cursor move.
                if (ts->csi_private == '?' && !ts->csi_intermediate && (byte == 'h' || byte == 'l'))
                    dispatchPrivateMode(buf, byte == 'h');
                else if (!ts->csi_private && !ts->csi_intermediate)
                    dispatchCsi(buf, (char)byte);
                ts->state = TPARSE_GROUND;
            }
            else if (byte < 0x20 && byte != 0x1b)
            {
                // C0 control inside a CSI: executed, sequence continues
                if (byte == '\r') carriageReturn(buf);
                else if (byte == '\n') lineFeed(buf);
                else if (byte == '\b') backspace(buf);
            }
            else if (byte == 0x1b)
            {
                ts->state = TPARSE_ESC; // aborted sequence, a new one starts
            }
            break;
        case TPARSE_OSC:
            if (byte == 0x07) { ts->state = TPARSE_GROUND; } // BEL terminator — done
            else if (byte == 0x1b) { ts->state = TPARSE_OSC_ESC; } // might be start of ST (ESC \)
            // else: still inside the OSC string — discard the byte, stay in TPARSE_OSC
            break;

        case TPARSE_OSC_ESC:
            if (byte == '\\') { ts->state = TPARSE_GROUND; } // confirmed ST — OSC done
            else { ts->state = TPARSE_OSC; } // wasn't ST, keep discarding
            break;
        }
    }
}
