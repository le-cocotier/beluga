/* 
* @file src/ansi_parser.c
* @author Giorgio
*/

#include "../include/ansi_parser.h"
#include "../include/buffer.h"
#include "../include/utf8.h"
#include <string.h>
#include <stdlib.h>

/**
 * @brief Ensure row->attrs is allocated and covers at least row->size elements.
 * Lazy allocation: file buffers never call this, so their rows stay NULL/untouched.
 */
static void ensureAttrs(Row* row)
{
    TermCellAttr* new_attrs = realloc(row->attrs, (row->size + 1) * sizeof(TermCellAttr));
    if (!new_attrs) return; // best-effort: skip attr tracking rather than crash
    row->attrs = new_attrs;
}

/**
 * @brief Get (creating if needed) the Row this buffer's parser cursor is on.
 */
static Row* currentRow(struct buffer_t* buf)
{
    TermParserState* ts = &buf->term_state;
    while (ts->cursor_y >= buf->numrows)
    {
        Utf8Char end_string;
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

    // Extend the row if the cursor is past its current end
    while (ts->cursor_x >= row->size)
    {
        bufferRowInsertChar /* your existing single-char insert */ (
            buf, row, row->size, c); // placeholder append; see note below
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
        ts->cursor_y++;
        ts->cursor_x = 0;

        int height = ts->pane_height > 0 ? ts->pane_height : 24; // fallback if never set
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
 * @brief Apply an SGR (Select Graphic Rendition) parameter to the parser's
 * current attribute state.
 */
static void applySgr(TermParserState* ts, int p)
{
    if (p == 0)              memset(&ts->current_attr, 0, sizeof(TermCellAttr));
    else if (p == 1)         ts->current_attr.bold = 1;
    else if (p == 4)         ts->current_attr.underline = 1;
    else if (p == 7)         ts->current_attr.reverse = 1;
    else if (p == 22)        ts->current_attr.bold = 0;
    else if (p == 24)        ts->current_attr.underline = 0;
    else if (p == 27)        ts->current_attr.reverse = 0;
    else if (p >= 30 && p <= 37)  ts->current_attr.fg = (p - 30) + 1;
    else if (p == 39)        ts->current_attr.fg = 0;
    else if (p >= 40 && p <= 47)  ts->current_attr.bg = (p - 40) + 1;
    else if (p == 49)        ts->current_attr.bg = 0;
    else if (p >= 90 && p <= 97)  ts->current_attr.fg = (p - 90) + 9;
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

    switch (final)
    {
        case 'A': // Cursor Up — relative to screen, clamp at screen top, not absolute 0
            ts->cursor_y -= (p0 ? p0 : 1);
            if (ts->cursor_y < ts->screen_top_row) ts->cursor_y = ts->screen_top_row;
            break;

        case 'B': // Cursor Down
            ts->cursor_y += (p0 ? p0 : 1);
            break;

        case 'C': ts->cursor_x += (p0 ? p0 : 1); break;
        case 'D': ts->cursor_x -= (p0 ? p0 : 1); if (ts->cursor_x < 0) ts->cursor_x = 0; break;

        case 'H': case 'f': // Cursor Position — row/col are screen-relative, 1-indexed
        {
            int row = ts->nparams > 0 ? ts->params[0] : 1;
            int col = ts->nparams > 1 ? ts->params[1] : 1;
            ts->cursor_y = ts->screen_top_row + (row > 0 ? row - 1 : 0);
            ts->cursor_x = (col > 0 ? col - 1 : 0);
            break;
        }
        case 'J': // Erase in display
            int mode = p0;
            int height = ts->pane_height > 0 ? ts->pane_height : 24;
            int screen_bottom = ts->screen_top_row + height - 1;

            if (mode == 0 || ts->nparams == 0)
            {
                // Erase from cursor to end of screen — cursor does NOT move
                if (ts->cursor_y < buf->numrows)
                {
                    Row* row = &buf->row[ts->cursor_y];
                    if (ts->cursor_x < row->size)
                    {
                        row->size = ts->cursor_x;
                        END_OF_STRING(row->characters[row->size]);
                    }
                }
                for (int y = ts->cursor_y + 1; y <= screen_bottom && y < buf->numrows; y++)
                {
                    buf->row[y].size = 0;
                    END_OF_STRING(buf->row[y].characters[0]);
                }
            }
            else if (mode == 2)
            {
                // Erase entire visible screen, cursor doesn't move either
                for (int y = ts->screen_top_row; y <= screen_bottom && y < buf->numrows; y++)
                {
                    buf->row[y].size = 0;
                    END_OF_STRING(buf->row[y].characters[0]);
                }
            }
            break;

        case 'K': // Erase in line — truncate current row from cursor_x onward
        {
            Row* row = currentRow(buf);
                int mode = p0; // 0 = cursor to end (default), 1 = start to cursor, 2 = whole line
                if (mode == 0 || ts->nparams == 0)
                {
                    if (ts->cursor_x < row->size) { row->size = ts->cursor_x; END_OF_STRING(row->characters[row->size]); }
                }
                else if (mode == 2)
                {
                    row->size = 0; END_OF_STRING(row->characters[0]);
                }
                // mode == 1 (start-to-cursor) omitted for now — rare in practice, needs a left-side blank-fill rather than truncation
                break;
            break;
        }

        case 'm': // SGR
            if (ts->nparams == 0)
                applySgr(ts, 0);
            else
                for (int i = 0; i < ts->nparams; i++)
                    applySgr(ts, ts->params[i]);
            break;

        case 's': ts->saved_cursor_x = ts->cursor_x; ts->saved_cursor_y = ts->cursor_y; break;
        case 'u': ts->cursor_x = ts->saved_cursor_x; ts->cursor_y = ts->saved_cursor_y; break;

        default:
            break; // unhandled CSI final byte — silently ignored for now
    }
}

void ansiParserFeed(struct buffer_t* buf, const unsigned char* data, size_t len)
{
    TermParserState* ts = &buf->term_state;

    for (size_t i = 0; i < len; i++)
    {
        unsigned char byte = data[i];

        switch (ts->state)
        {
            case TPARSE_GROUND:
                if (byte == 0x1b) { ts->state = TPARSE_ESC; break; }
                if (byte == '\r') { carriageReturn(buf); break; }
                if (byte == '\n') { newLine(buf); break; }
                if (byte == '\b' || byte == 0x7f) { backspace(buf); break; }
                if (byte == '\t')
                {
                    Utf8Char space = { .character = {' ',0,0,0}, .length = 1 };
                    for (int t = 0; t < 4; t++) putChar(buf, &space); // simple fixed tab-stop
                    break;
                }

                if (byte < 0x20) break; // other control chars: ignore for now

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
                    Utf8Char c;
                    memcpy(c.character, ts->utf8_pending, ts->utf8_pending_len);
                    c.length = ts->utf8_pending_len;
                    putChar(buf, &c);
                    ts->utf8_pending_len = 0;
                }
                break;

            case TPARSE_ESC:
            if (byte == '[') { ts->state = TPARSE_CSI; ts->nparams = 0; memset(ts->params, 0, sizeof(ts->params)); }
                else if (byte == ']') { ts->state = TPARSE_OSC; } // <-- new
                else if (byte == '7') { ts->saved_cursor_x = ts->cursor_x; ts->saved_cursor_y = ts->cursor_y; ts->state = TPARSE_GROUND; }
                else if (byte == '8') { ts->cursor_x = ts->saved_cursor_x; ts->cursor_y = ts->saved_cursor_y; ts->state = TPARSE_GROUND; }
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
                else if (byte >= 0x40 && byte <= 0x7E) // final byte
                {
                    dispatchCsi(buf, (char) byte);
                    ts->state = TPARSE_GROUND;
                }
                // else: ignore intermediate bytes (rare, e.g. '?' for private modes)
                break;
                case TPARSE_OSC:
                    if (byte == 0x07) { ts->state = TPARSE_GROUND; }        // BEL terminator — done
                    else if (byte == 0x1b) { ts->state = TPARSE_OSC_ESC; }  // might be start of ST (ESC \)
                    // else: still inside the OSC string — discard the byte, stay in TPARSE_OSC
                    break;

                case TPARSE_OSC_ESC:
                    if (byte == '\\') { ts->state = TPARSE_GROUND; }        // confirmed ST — OSC done
                    else { ts->state = TPARSE_OSC; }                        // wasn't ST, keep discarding
                    break;
        }
    }
}
