// include/ansi_parser.h
#ifndef ANSI_PARSER_H_
#define ANSI_PARSER_H_

#include "data.h"

void ansiParserFeed(struct buffer_t* buf, const unsigned char* data, size_t len);

#endif
