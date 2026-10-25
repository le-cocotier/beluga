/**
 * @file fuzz_ansi_parser.c
 * @brief Fuzz target: arbitrary program output into a terminal pane's
 * parser (ansi_parser.c)
 * @details The input is split in two at a point chosen by its first byte,
 * so sequences cut across two reads are covered too.
 */

#include "../../include/ansi_parser.h"
#include "../../include/buffer.h"
#include "../../include/galloc.h"
#include "../test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Buffer *buf;
  size_t split;
  static int initialized = 0;
  if (!initialized) {
    testInit();
    initialized = 1;
  }
  if (size == 0)
    return 0;
  buf = gcalloc(1, sizeof(Buffer));
  buf->type = TERMINAL_BUFF;
  buf->state = READ_AND_WRITE;
  buf->term_state.pane_height = 1 + data[0] % 40;
  // Most inputs also get a width, so the wrapping is fuzzed too.
  buf->term_state.pane_width = data[0] % 4 ? 1 + (data[0] * 7) % 80 : 0;
  split = size > 1 ? (size_t)data[0] % size : 0;
  ansiParserFeed(buf, data + 1, split > 0 ? split - 1 : 0);
  ansiParserFeed(buf, data + (split > 0 ? split : 1), size - (split > 0 ? split : 1));
  bufferFree(buf);
  return 0;
}
