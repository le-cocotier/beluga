// src/term_render.c
#include "../include/term_render.h"
#include "../include/galloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void appendSgrFor(char* out, int* pos, const int buf_size, const TermCellAttr a)
{
    // Only emit a code if this cell has any non-default attribute;
    // ground-state rendering can just skip the color set/reset pair.
    char codes[64] = "\x1b[0"; // always start from reset to avoid state leaking between cells
    int n = 3;
    if (a.bold)      n += snprintf(codes + n, sizeof(codes) - n, ";1");
    if (a.underline) n += snprintf(codes + n, sizeof(codes) - n, ";4");
    if (a.reverse)   n += snprintf(codes + n, sizeof(codes) - n, ";7");
    if (a.fg >= 1 && a.fg <= 8)  n += snprintf(codes + n, sizeof(codes) - n, ";%d", 30 + (a.fg - 1));
    if (a.fg >= 9 && a.fg <= 16) n += snprintf(codes + n, sizeof(codes) - n, ";%d", 90 + (a.fg - 9));
    if (a.bg >= 1 && a.bg <= 8)  n += snprintf(codes + n, sizeof(codes) - n, ";%d", 40 + (a.bg - 1));
    if (a.bg >= 9 && a.bg <= 16) n += snprintf(codes + n, sizeof(codes) - n, ";%d", 100 + (a.bg - 9));
    n += snprintf(codes + n, sizeof(codes) - n, "m");

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
    if (x_offset < 0) x_offset = 0;
    if (width < 0) width = 0;

    start = x_offset;
    end = x_offset + width;
    if (start > row->size) start = row->size;
    if (end > row->size) end = row->size;

    visible = end - start;
    if (visible < 0) visible = 0;

    buf_size = (visible + 1) * 32 + 64;
    result = galloc(buf_size);
    if (!result) { *length = 0; return NULL; }

    pos = 0;
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
