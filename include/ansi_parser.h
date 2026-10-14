// include/ansi_parser.h
#ifndef ANSI_PARSER_H_
#define ANSI_PARSER_H_

#include "data.h"

void ansiParserFeed(struct buffer_t* buf, const unsigned char* data, size_t len);
// The character index of the parser's cursor on its row (the cursor itself
// counts screen columns); past the row's end, as if blanks filled the gap.
int ansiParserCursorIndex(const struct buffer_t* buf);
// Frees what the parser holds besides the buffer's rows (the normal screen,
// while the alternate one is shown). bufferFree() calls it.
void ansiParserRelease(struct buffer_t* buf);

#endif
