// src/term_render.c
#include "../include/term_render.h"
#include "../include/galloc.h"
#include "../include/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One colour's SGR parameters: `base` is 30 for the foreground, 40 for the
// background (see TermCellAttr for the encoding).
static int appendColor(char* codes, const int size, const int base, const unsigned int color)
{
    if (color == 0)
        return 0;
    if (color & TERM_COLOR_RGB)
        return snprintf(codes, size, ";%d;2;%u;%u;%u", base + 8, (color >> 16) & 0xFF,
                        (color >> 8) & 0xFF, color & 0xFF);
    if (color <= 8)
        return snprintf(codes, size, ";%u", base + color - 1);
    if (color <= 16)
        return snprintf(codes, size, ";%u", base + 60 + color - 9);
    return snprintf(codes, size, ";%d;5;%u", base + 8, color - 1);
}

static void appendSgrFor(char* out, int* pos, const int buf_size, const TermCellAttr a)
{
    char codes[96] = "\x1b[0"; // always start from reset to avoid state leaking between cells
    int n = 3;
    if (a.bold)      n += snprintf(codes + n, sizeof(codes) - n, ";1");
    if (a.underline) n += snprintf(codes + n, sizeof(codes) - n, ";4");
    if (a.reverse)   n += snprintf(codes + n, sizeof(codes) - n, ";7");
    n += appendColor(codes + n, (int) sizeof(codes) - n, 30, a.fg);
    n += appendColor(codes + n, (int) sizeof(codes) - n, 40, a.bg);
    snprintf(codes + n, sizeof(codes) - n, "m");

    *pos += snprintf(out + *pos, buf_size - *pos, "%s", codes);
}

char* termRenderRow(const Row* row, int* length, int x_offset, int width)
{
    int start;
    int end;
    int visible;
    int buf_size;
    char* result;
    int pos;
    TermCellAttr last_attr;
    int i;
    TermCellAttr attr;
    Utf8Span span;
    if (x_offset < 0) x_offset = 0;
    if (width < 0) width = 0;

    // Screen columns to characters (wide characters take two columns).
    utf8VisibleSpan(row->characters, row->size, x_offset, width, &span);
    start = span.start;
    end = span.end;

    visible = end - start;
    if (visible < 0) visible = 0;

    // Per cell: its bytes and, at worst, a full SGR (two truecolours).
    buf_size = (visible + 1) * 64 + 64 + span.lead_pad;
    result = galloc(buf_size);
    if (!result) { *length = 0; return NULL; }

    pos = 0;
    for (i = 0; i < span.lead_pad; i++)
        result[pos++] = ' ';
    memset(&last_attr, 0xFF, sizeof(last_attr)); // force a mismatch on first cell

    for (i = start; i < end; i++)
    {
        attr = row->attrs ? row->attrs[i] : (TermCellAttr){0};

        if (attr.fg != last_attr.fg || attr.bg != last_attr.bg ||
            attr.bold != last_attr.bold || attr.underline != last_attr.underline ||
            attr.reverse != last_attr.reverse)
        {
            appendSgrFor(result, &pos, buf_size, attr);
            last_attr = attr;
        }

        memcpy(&result[pos], row->characters[i].character, row->characters[i].length);
        pos += row->characters[i].length;
    }

    pos += snprintf(result + pos, buf_size - pos, "\x1b[0m"); // reset at end of line
    result[pos] = '\0';
    *length = pos;
    return result;
}
