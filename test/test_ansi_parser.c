/**
 * @file test_ansi_parser.c
 * @brief Unit tests for the terminal output parser (ansi_parser.c)
 */

#include "test.h"
#include "../include/ansi_parser.h"
#include "../include/buffer.h"
#include "../include/galloc.h"
#include "../include/utf8.h"

static Buffer *terminalBuffer(void) {
  Buffer *buf = gcalloc(1, sizeof(Buffer));
  buf->type = TERMINAL_BUFF;
  buf->state = READ_AND_WRITE;
  buf->term_state.pane_height = 10;
  return buf;
}

static void feed(Buffer *buf, const char *s) {
  ansiParserFeed(buf, (const unsigned char *)s, strlen(s));
}

// Row `row` as a C string (caller frees).
static char *rowText(Buffer *buf, int row) {
  if (row >= buf->numrows)
    return NULL;
  return (char *)utf8ToNString(buf->row[row].characters, buf->row[row].size);
}

#define CHECK_ROW(buf, row, expected)                                          \
  do {                                                                         \
    char *r_ = rowText(buf, row);                                              \
    CHECK_STR(r_, expected);                                                   \
    gfree(r_);                                                                 \
  } while (0)

static void testLines(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "hello\r\nw\xc3\xb6rld");
  CHECK_INT(buf->numrows, 2);
  CHECK_ROW(buf, 0, "hello");
  CHECK_ROW(buf, 1, "w\xc3\xb6rld");
  CHECK_INT(buf->term_state.cursor_x, 5);
  bufferFree(buf);
}

static void testSplitSequence(void) {
  // A sequence cut between two reads still parses.
  Buffer *buf = terminalBuffer();
  feed(buf, "a\x1b[");
  feed(buf, "31mb\x1b[0mc");
  CHECK_ROW(buf, 0, "abc");
  CHECK(buf->row[0].attrs != NULL);
  CHECK_INT(buf->row[0].attrs[0].fg, 0);
  CHECK(buf->row[0].attrs[1].fg != 0);
  CHECK_INT(buf->row[0].attrs[2].fg, 0);
  bufferFree(buf);
}

static void testCarriageReturnOverwrite(void) {
  // Progress bars redraw a line with CR.
  Buffer *buf = terminalBuffer();
  feed(buf, "10%\r50%\r\x1b[K100%");
  CHECK_ROW(buf, 0, "100%");
  bufferFree(buf);
}

static void testCursorPosition(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "\x1b[3;4Hx");
  CHECK_INT(buf->term_state.cursor_y, 2);
  CHECK_INT(buf->term_state.cursor_x, 4);
  CHECK_ROW(buf, 2, "   x");
  bufferFree(buf);
}

static void testEraseDisplay(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "one\r\ntwo\r\n\x1b[2J\x1b[Hnew");
  CHECK_ROW(buf, 0, "new");
  CHECK_ROW(buf, 1, "");
  bufferFree(buf);
}

static void testTabStops(void) {
  // ls-style columns: every tab lands on a multiple of 8.
  Buffer *buf = terminalBuffer();
  feed(buf, "a\tbb\tccc\r\n12345678\tx");
  CHECK_ROW(buf, 0, "a       bb      ccc");
  CHECK_ROW(buf, 1, "12345678        x");
  bufferFree(buf);
}

static void testTabKeepsText(void) {
  // A tab over existing text moves the cursor without erasing it.
  Buffer *buf = terminalBuffer();
  feed(buf, "abcdefghij\r\tX");
  CHECK_ROW(buf, 0, "abcdefghXj");
  bufferFree(buf);
}

static void testEraseToCursor(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "abcdef\x1b[3G\x1b[1K");
  CHECK_ROW(buf, 0, "   def");
  CHECK_INT(buf->term_state.cursor_x, 2);
  bufferFree(buf);
}

static void testEraseDeleteInsertChars(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "abcdef\x1b[2G\x1b[2X");
  CHECK_ROW(buf, 0, "a  def");
  feed(buf, "\r\x1b[2P");
  CHECK_ROW(buf, 0, " def");
  feed(buf, "\x1b[2@");
  CHECK_ROW(buf, 0, "   def");
  bufferFree(buf);
}

static void testControlsAndPrivateModes(void) {
  // BEL and other C0 controls never reach the text, and DEC private
  // sequences aren't mistaken for erase/cursor commands.
  Buffer *buf = terminalBuffer();
  feed(buf, "a\x07" "b\x0e\x0f" "c\x1b[?25l\x1b[?1049h\x1b[?2Jd");
  CHECK_ROW(buf, 0, "abcd");
  feed(buf, "\x0b" "e");
  CHECK_INT(buf->numrows, 2);
  CHECK_ROW(buf, 1, "e");
  bufferFree(buf);
}

static void testAbsolutePositions(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "\x1b[3dx\x1b[1;1Hy\x1b[2Ez\x1b[Fw");
  CHECK_ROW(buf, 0, "y");
  CHECK_ROW(buf, 1, "w");
  CHECK_ROW(buf, 2, "z");
  bufferFree(buf);
}

int main(void) {
  testInit();
  testLines();
  testSplitSequence();
  testCarriageReturnOverwrite();
  testCursorPosition();
  testEraseDisplay();
  testTabStops();
  testTabKeepsText();
  testEraseToCursor();
  testEraseDeleteInsertChars();
  testControlsAndPrivateModes();
  testAbsolutePositions();
  return testDone();
}
