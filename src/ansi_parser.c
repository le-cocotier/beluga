/* 
* @file src/ansi_parser.c
* @author Giorgio
*/

#include "../include/ansi_parser.h"
#include "../include/galloc.h"
#include "../include/buffer.h"
#include "../include/utf8.h"
#include <string.h>

/**
 * @brief Ensure row->attrs is allocated and covers at least row->size elements.
 * Lazy allocation: file buffers never call this, so their rows stay NULL/untouched.
 */
static void ensureAttrs(Row* row)
{
    TermCellAttr* new_attrs = grealloc(row->attrs, (row->size + 1) * sizeof(TermCellAttr));
    if (!new_attrs) return; // best-effort: skip attr tracking rather than crash
    row->attrs = new_attrs;
}

/**
 * @brief Get (creating if needed) the Row this buffer's parser cursor is on.
 */
static Row* currentRow(struct buffer_t* buf)
{
    const TermParserState* ts = &buf->term_state;
    Utf8Char end_string;
    while (ts->cursor_y >= buf->numrows)
    {
        END_OF_STRING(end_string);
        bufferInsertRow(buf, buf->numrows, &end_string, 0);
    }
    return &buf->row[ts->cursor_y];
}

/**
 * @brief Write one decoded Utf8Char at the parser's current cursor position,
 * with the parser's current SGR attribute, then advance the cursor (wrapping
 * to a new row if past pane width — width isn't known here, so we wrap on a
 * generous default; real width-aware wrapping happens via terminal resize
 * events feeding cols into ptySessionResize).
 */
static void putChar(struct buffer_t* buf, Utf8Char* c)
{
    TermParserState* ts = &buf->term_state;
    Row* row = currentRow(buf);
    Utf8Char space;
    TermCellAttr blank_attr;

    // Extend the row if the cursor is past its current end. A cursor moved
    // beyond the end (CUP or CUF onto a short or blank line, as full-screen
    // programs do) leaves blanks in between, as on a real terminal.
    if (ts->cursor_x >= row->size)
    {
        space.character[0] = ' ';
        space.length = 1;
        memset(&blank_attr, 0, sizeof(blank_attr));
        while (row->size < ts->cursor_x)
        {
            bufferRowInsertChar(buf, row, row->size, &space);
            ensureAttrs(row);
            row->attrs[row->size - 1] = blank_attr;
        }
        bufferRowInsertChar(buf, row, row->size, c); // append at the row's current end
        ensureAttrs(row);
        row->attrs[row->size - 1] = ts->current_attr;
        ts->cursor_x = row->size;
        return;
    }

    // Overwrite in place (cursor moved backward via CUB/CUP, typical for
    // progress bars, prompts redrawing themselves, etc.)
    memcpy(row->characters[ts->cursor_x].character, c->character, c->length);
    row->characters[ts->cursor_x].length = c->length;
    ensureAttrs(row);
    row->attrs[ts->cursor_x] = ts->current_attr;
    ts->cursor_x++;
}

static void newLine(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    int height;
    ts->cursor_y++;
    ts->cursor_x = 0;

    height = ts->pane_height > 0 ? ts->pane_height : 24; // fallback if never set
    if (ts->cursor_y >= ts->screen_top_row + height)
        ts->screen_top_row = ts->cursor_y - height + 1;
}

static void carriageReturn(struct buffer_t* buf)
{
    buf->term_state.cursor_x = 0;
}

static void backspace(struct buffer_t* buf)
{
    if (buf->term_state.cursor_x > 0)
        buf->term_state.cursor_x--;
}

/**
 * @brief Overwrite the cell at column x of the current row with a blank
 * carrying the current attribute. Cells past the row's end are already
 * blank on screen, so they are left alone.
 */
static void blankCell(struct buffer_t* buf, Row* row, int x)
{
    if (x < 0 || x >= row->size) return;
    row->characters[x].character[0] = ' ';
    row->characters[x].length = 1;
    ensureAttrs(row);
    row->attrs[x] = buf->term_state.current_attr;
}

/**
 * @brief HT: move to the next 8-column tab stop. Like a real terminal it
 * only moves the cursor: text it passes over stays, and a stop past the
 * row's end is padded with blanks by the next putChar().
 */
static void horizontalTab(struct buffer_t* buf)
{
    buf->term_state.cursor_x = (buf->term_state.cursor_x / 8 + 1) * 8;
}

/**
 * @brief Apply an SGR (Select Graphic Rendition) parameter to the parser's
 * current attribute state.
 */
static void applySgr(TermParserState* ts, int p)
{
    if (p == 0)
        memset(&ts->current_attr, 0, sizeof(TermCellAttr));
    else if (p == 1) ts->current_attr.bold = 1;
    else if (p == 4) ts->current_attr.underline = 1;
    else if (p == 7) ts->current_attr.reverse = 1;
    else if (p == 22) ts->current_attr.bold = 0;
    else if (p == 24) ts->current_attr.underline = 0;
    else if (p == 27) ts->current_attr.reverse = 0;
    else if (p >= 30 && p <= 37) ts->current_attr.fg = (p - 30) + 1;
    else if (p == 39) ts->current_attr.fg = 0;
    else if (p >= 40 && p <= 47) ts->current_attr.bg = (p - 40) + 1;
    else if (p == 49) ts->current_attr.bg = 0;
    else if (p >= 90 && p <= 97) ts->current_attr.fg = (p - 90) + 9;
    else if (p >= 100 && p <= 107) ts->current_attr.bg = (p - 100) + 9;
    // 38;5;n / 38;2;r;g;b (256-color / truecolor) not yet handled — see notes
}

/**
 * @brief Dispatch a fully-parsed CSI sequence (params + final byte) to the
 * appropriate terminal action.
 */
static void dispatchCsi(struct buffer_t* buf, char final)
{
    TermParserState* ts = &buf->term_state;
    int p0 = ts->nparams > 0 ? ts->params[0] : 0;
    int row;
    int col;
    int mode;
    int height;
    int screen_bottom;
    Row* erase_row;
    int y;
    int line_mode;
    int i;
    Utf8Char space = {.character = {' ', 0, 0, 0}, .length = 1};

    switch (final)
    {
    case 'A': // Cursor Up — relative to screen, clamp at screen top, not absolute 0
        ts->cursor_y -= (p0 ? p0 : 1);
        if (ts->cursor_y < ts->screen_top_row) ts->cursor_y = ts->screen_top_row;
        break;

    case 'B': // Cursor Down
        ts->cursor_y += (p0 ? p0 : 1);
        break;

    case 'C': ts->cursor_x += (p0 ? p0 : 1);
        break;
    case 'D': ts->cursor_x -= (p0 ? p0 : 1);
        if (ts->cursor_x < 0) ts->cursor_x = 0;
        break;

    case 'H':
    case 'f': // Cursor Position — row/col are screen-relative, 1-indexed
        {
            row = ts->nparams > 0 ? ts->params[0] : 1;
            col = ts->nparams > 1 ? ts->params[1] : 1;
            ts->cursor_y = ts->screen_top_row + (row > 0 ? row - 1 : 0);
            ts->cursor_x = (col > 0 ? col - 1 : 0);
            break;
        }
    case 'J': // Erase in display
        mode = p0;
        height = ts->pane_height > 0 ? ts->pane_height : 24;
        screen_bottom = ts->screen_top_row + height - 1;

        if (mode == 0 || ts->nparams == 0)
        {
            // Erase from cursor to end of screen — cursor does NOT move
            if (ts->cursor_y < buf->numrows)
            {
                erase_row = &buf->row[ts->cursor_y];
                if (ts->cursor_x < erase_row->size)
                {
                    erase_row->size = ts->cursor_x;
                    END_OF_STRING(erase_row->characters[erase_row->size]);
                }
            }
            for (y = ts->cursor_y + 1; y <= screen_bottom && y < buf->numrows; y++)
            {
                buf->row[y].size = 0;
                END_OF_STRING(buf->row[y].characters[0]);
            }
        }
        else if (mode == 2)
        {
            // Erase entire visible screen, cursor doesn't move either
            for (y = ts->screen_top_row; y <= screen_bottom && y < buf->numrows; y++)
            {
                buf->row[y].size = 0;
                END_OF_STRING(buf->row[y].characters[0]);
            }
        }
        break;

    case 'K': // Erase in line — truncate current row from cursor_x onward
        {
            erase_row = currentRow(buf);
            line_mode = p0; // 0 = cursor to end (default), 1 = start to cursor, 2 = whole line
            if (line_mode == 0 || ts->nparams == 0)
            {
                if (ts->cursor_x < erase_row->size)
                {
                    erase_row->size = ts->cursor_x;
                    END_OF_STRING(erase_row->characters[erase_row->size]);
                }
            }
            else if (line_mode == 1)
            {
                // Start of line through the cursor, inclusive: blank those
                // cells, keep what's right of the cursor.
                for (i = 0; i <= ts->cursor_x; i++)
                    blankCell(buf, erase_row, i);
            }
            else if (line_mode == 2)
            {
                erase_row->size = 0;
                END_OF_STRING(erase_row->characters[0]);
            }
            break;
        }

    case 'X': // Erase Character — blank n cells from the cursor, cursor stays
        erase_row = currentRow(buf);
        for (i = 0; i < (p0 ? p0 : 1); i++)
            blankCell(buf, erase_row, ts->cursor_x + i);
        break;

    case 'P': // Delete Character — shift the rest of the line left by n
        erase_row = currentRow(buf);
        for (i = 0; i < (p0 ? p0 : 1) && ts->cursor_x < erase_row->size; i++)
            bufferRowDelChar(buf, erase_row, ts->cursor_x);
        if (erase_row->attrs && ts->cursor_x < erase_row->size)
            memmove(&erase_row->attrs[ts->cursor_x], &erase_row->attrs[ts->cursor_x + i],
                    (erase_row->size - ts->cursor_x) * sizeof(TermCellAttr));
        break;

    case '@': // Insert Character — open n blanks at the cursor
        {
            erase_row = currentRow(buf);
            if (ts->cursor_x >= erase_row->size) break; // nothing to push right
            for (i = 0; i < (p0 ? p0 : 1); i++)
                bufferRowInsertChar(buf, erase_row, ts->cursor_x, &space);
            ensureAttrs(erase_row);
            memmove(&erase_row->attrs[ts->cursor_x + i], &erase_row->attrs[ts->cursor_x],
                    (erase_row->size - ts->cursor_x - i) * sizeof(TermCellAttr));
            for (i = 0; i < (p0 ? p0 : 1); i++)
                erase_row->attrs[ts->cursor_x + i] = ts->current_attr;
            break;
        }

    case 'G': // Cursor Horizontal Absolute — 1-indexed column
    case '`':
        ts->cursor_x = (p0 > 0 ? p0 - 1 : 0);
        break;

    case 'd': // Line Position Absolute — 1-indexed, screen-relative
        ts->cursor_y = ts->screen_top_row + (p0 > 0 ? p0 - 1 : 0);
        break;

    case 'E': // Cursor Next Line
        ts->cursor_y += (p0 ? p0 : 1);
        ts->cursor_x = 0;
        break;

    case 'F': // Cursor Previous Line
        ts->cursor_y -= (p0 ? p0 : 1);
        if (ts->cursor_y < ts->screen_top_row) ts->cursor_y = ts->screen_top_row;
        ts->cursor_x = 0;
        break;

    case 'm': // SGR
        if (ts->nparams == 0)
            applySgr(ts, 0);
        else
            for (i = 0; i < ts->nparams; i++)
                applySgr(ts, ts->params[i]);
        break;

    case 's': ts->saved_cursor_x = ts->cursor_x;
        ts->saved_cursor_y = ts->cursor_y;
        break;
    case 'u': ts->cursor_x = ts->saved_cursor_x;
        ts->cursor_y = ts->saved_cursor_y;
        break;

    default:
        break; // other final bytes (scroll regions, modes, ...) don't change the text
    }
}

void ansiParserFeed(struct buffer_t* buf, const unsigned char* data, size_t len)
{
    TermParserState* ts = &buf->term_state;
    size_t i;
    unsigned char byte;
    Utf8Char c;

    for (i = 0; i < len; i++)
    {
        byte = data[i];

        switch (ts->state)
        {
        case TPARSE_GROUND:
            if (byte == 0x1b)
            {
                ts->state = TPARSE_ESC;
                break;
            }
            if (byte == '\r')
            {
                carriageReturn(buf);
                break;
            }
            if (byte == '\n')
            {
                newLine(buf);
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
            if (byte == 0x0b || byte == 0x0c) // VT and FF move down like LF
            {
                newLine(buf);
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
                memcpy(c.character, ts->utf8_pending, ts->utf8_pending_len);
                c.length = (char) ts->utf8_pending_len;
                putChar(buf, &c);
                ts->utf8_pending_len = 0;
            }
            break;

        case TPARSE_ESC:
            if (byte == '[')
            {
                ts->state = TPARSE_CSI;
                ts->nparams = 0;
                ts->csi_private = 0;
                memset(ts->params, 0, sizeof(ts->params));
            }
            else if (byte == ']') { ts->state = TPARSE_OSC; } // <-- new
            else if (byte == '7')
            {
                ts->saved_cursor_x = ts->cursor_x;
                ts->saved_cursor_y = ts->cursor_y;
                ts->state = TPARSE_GROUND;
            }
            else if (byte == '8')
            {
                ts->cursor_x = ts->saved_cursor_x;
                ts->cursor_y = ts->saved_cursor_y;
                ts->state = TPARSE_GROUND;
            }
            else { ts->state = TPARSE_GROUND; }
            break;

        case TPARSE_CSI:
            if (byte >= '0' && byte <= '9')
            {
                if (ts->nparams == 0) ts->nparams = 1;
                ts->params[ts->nparams - 1] = ts->params[ts->nparams - 1] * 10 + (byte - '0');
            }
            else if (byte == ';')
            {
                if (ts->nparams < CSI_MAX_PARAMS) ts->nparams++;
            }
            else if (byte >= 0x3C && byte <= 0x3F) // '<' '=' '>' '?' prefix
            {
                ts->csi_private = 1;
            }
            else if (byte >= 0x40 && byte <= 0x7E) // final byte
            {
                // DEC private sequences (?25l cursor visibility, ?1049h
                // alternate screen, ...) share final bytes with the standard
                // ones; none of them changes the text, so don't misread them
                // as erase or cursor moves.
                if (!ts->csi_private)
                    dispatchCsi(buf, (char)byte);
                ts->state = TPARSE_GROUND;
            }
            else if (byte < 0x20 && byte != 0x1b)
            {
                // C0 control inside a CSI: executed, sequence continues
                if (byte == '\r') carriageReturn(buf);
                else if (byte == '\n') newLine(buf);
                else if (byte == '\b') backspace(buf);
            }
            else if (byte == 0x1b)
            {
                ts->state = TPARSE_ESC; // aborted sequence, a new one starts
            }
            // else: other intermediate bytes (0x20-0x2F) are ignored
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
