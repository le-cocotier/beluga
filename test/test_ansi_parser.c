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
  // sequences aren't mistaken for erase/cursor commands. VT moves down
  // like LF, which (unlike CR LF) keeps the column.
  Buffer *buf = terminalBuffer();
  feed(buf, "a\x07" "b\x0e\x0f" "c\x1b[?25l\x1b[?2004h\x1b[?2Jd");
  CHECK_ROW(buf, 0, "abcd");
  feed(buf, "\x0b" "e");
  CHECK_INT(buf->numrows, 2);
  CHECK_ROW(buf, 1, "    e");
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

static void testCharsetDesignation(void) {
  // ESC ( B (ncurses' sgr0 starts with it) is swallowed whole.
  Buffer *buf = terminalBuffer();
  feed(buf, "a\x1b(Bb\x1b)0c\x1b#8d");
  CHECK_ROW(buf, 0, "abcd");
  bufferFree(buf);
}

static void testWideCharacterColumns(void) {
  // The cursor counts columns: after two wide characters it is at column
  // 4, and a position on the line lands where the terminal shows it.
  Buffer *buf = terminalBuffer();
  feed(buf, "\xe4\xb8\xad\xe6\x96\x87x");
  CHECK_INT(buf->term_state.cursor_x, 5);
  CHECK_INT(ansiParserCursorIndex(buf), 3);
  feed(buf, "\x1b[1;5Hy");
  CHECK_ROW(buf, 0, "\xe4\xb8\xad\xe6\x96\x87y");
  // Overwriting half of a wide character leaves a blank in the other half.
  feed(buf, "\x1b[1;2Hz");
  CHECK_ROW(buf, 0, " z\xe6\x96\x87y");
  // A combining mark joins the character before it.
  feed(buf, "\x1b[1;6He\xcc\x81!");
  CHECK_ROW(buf, 0, " z\xe6\x96\x87ye\xcc\x81!");
  CHECK_INT(buf->term_state.cursor_x, 7);
  bufferFree(buf);
}

static void testColors(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "\x1b[31ma\x1b[38;5;208mb\x1b[38;2;1;2;3;48;5;4mc\x1b[94;101md\x1b[0me");
  CHECK_INT(buf->row[0].attrs[0].fg, 2);
  CHECK_INT(buf->row[0].attrs[1].fg, 209);
  CHECK_INT(buf->row[0].attrs[2].fg, TERM_COLOR_RGB | 0x010203);
  CHECK_INT(buf->row[0].attrs[2].bg, 5);
  CHECK_INT(buf->row[0].attrs[3].fg, 13);
  CHECK_INT(buf->row[0].attrs[3].bg, 10);
  CHECK_INT(buf->row[0].attrs[4].fg, 0);
  CHECK_INT(buf->row[0].attrs[4].bg, 0);
  // The colon form, and an underline colour that's skipped.
  feed(buf, "\x1b[38:5:21;58;5;3mf");
  CHECK_INT(buf->row[0].attrs[5].fg, 22);
  bufferFree(buf);
}

static void testAlternateScreen(void) {
  // vim, less, htop: the alternate screen starts blank, and leaving it
  // brings back the shell's screen and cursor as they were.
  Buffer *buf = terminalBuffer();
  feed(buf, "$ vim\r\n");
  feed(buf, "\x1b[?1049h\x1b[Hfull\x1b[5;1Hscreen");
  CHECK_INT(buf->numrows, 10);
  CHECK_ROW(buf, 0, "full");
  CHECK_ROW(buf, 4, "screen");
  feed(buf, "\x1b[?1049l");
  CHECK_INT(buf->numrows, 1);
  CHECK_ROW(buf, 0, "$ vim");
  CHECK_INT(buf->term_state.cursor_y, 1);
  CHECK_INT(buf->term_state.cursor_x, 0);
  // Freed while still on the alternate screen: nothing leaks (ASan).
  feed(buf, "\x1b[?1049h");
  bufferFree(buf);
}

static void testScrollRegion(void) {
  // A status line under a scrolling region (less, vim): LF at the region's
  // bottom scrolls only the region.
  Buffer *buf = terminalBuffer();
  feed(buf, "\x1b[?1049h\x1b[10;1Hstatus\x1b[1;9r");
  CHECK_INT(buf->term_state.cursor_y, 0);
  feed(buf, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10");
  CHECK_ROW(buf, 0, "2");
  CHECK_ROW(buf, 8, "10");
  CHECK_ROW(buf, 9, "status");
  CHECK_INT(buf->numrows, 10);
  // Reverse index at the top scrolls the region down.
  feed(buf, "\x1b[1;1H\x1bMtop");
  CHECK_ROW(buf, 0, "top");
  CHECK_ROW(buf, 1, "2");
  CHECK_ROW(buf, 8, "9");
  CHECK_ROW(buf, 9, "status");
  bufferFree(buf);
}

static void testInsertDeleteLines(void) {
  Buffer *buf = terminalBuffer();
  feed(buf, "\x1b[?1049ha\r\nb\r\nc\r\nd\x1b[2;1H\x1b[L");
  CHECK_ROW(buf, 0, "a");
  CHECK_ROW(buf, 1, "");
  CHECK_ROW(buf, 2, "b");
  CHECK_ROW(buf, 4, "d");
  feed(buf, "\x1b[2M");
  CHECK_ROW(buf, 1, "c");
  CHECK_ROW(buf, 2, "d");
  CHECK_ROW(buf, 3, "");
  CHECK_INT(buf->numrows, 10);
  // Scroll Up / Down move the whole region.
  feed(buf, "\x1b[S");
  CHECK_ROW(buf, 0, "c");
  feed(buf, "\x1b[2T");
  CHECK_ROW(buf, 2, "c");
  bufferFree(buf);
}

static void testScrollbackOnNormalScreen(void) {
  // Without a region, the normal screen keeps what scrolls off as
  // scrollback, as before.
  Buffer *buf = terminalBuffer();
  int i;
  for (i = 0; i < 15; i++)
    feed(buf, "line\r\n");
  CHECK_INT(buf->term_state.screen_top_row, 6);
  CHECK_INT(buf->term_state.cursor_y, 15);
  CHECK_INT(buf->numrows, 15);
  bufferFree(buf);
}

static void testWrap(void) {
  // At the pane's width a line goes on to the next row, xterm style: the
  // cursor waits on the last column until the next character.
  Buffer *buf = terminalBuffer();
  buf->term_state.pane_width = 5;
  feed(buf, "abcde");
  CHECK_INT(buf->numrows, 1);
  CHECK_INT(buf->term_state.cursor_x, 4);
  feed(buf, "fg");
  CHECK_ROW(buf, 0, "abcde");
  CHECK_ROW(buf, 1, "fg");
  // Filling the last column then CR LF leaves no blank line between.
  feed(buf, "\r\nhijkl\r\nm");
  CHECK_ROW(buf, 2, "hijkl");
  CHECK_ROW(buf, 3, "m");
  // A cursor move drops the pending wrap; colours don't.
  feed(buf, "\r\nnopqr\x1b[1Gs");
  CHECK_ROW(buf, 4, "sopqr");
  feed(buf, "\r\ntuvwx\x1b[31my");
  CHECK_ROW(buf, 5, "tuvwx");
  CHECK_ROW(buf, 6, "y");
  // A wide character that doesn't fit in the last column wraps whole.
  feed(buf, "\r\nabcd\xe4\xb8\xad");
  CHECK_ROW(buf, 7, "abcd");
  CHECK_ROW(buf, 8, "\xe4\xb8\xad");
  CHECK_INT(buf->term_state.cursor_x, 2);
  // Cursor moves stop at the last column.
  feed(buf, "\x1b[99C");
  CHECK_INT(buf->term_state.cursor_x, 4);
  bufferFree(buf);
}

static void testWrapScrolls(void) {
  // Wrapping on the bottom row scrolls, like a line feed.
  Buffer *buf = terminalBuffer();
  buf->term_state.pane_width = 3;
  buf->term_state.pane_height = 2;
  feed(buf, "abcdefgh");
  CHECK_ROW(buf, 0, "abc");
  CHECK_ROW(buf, 2, "gh");
  CHECK_INT(buf->term_state.screen_top_row, 1);
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
  testCharsetDesignation();
  testWideCharacterColumns();
  testColors();
  testAlternateScreen();
  testScrollRegion();
  testInsertDeleteLines();
  testScrollbackOnNormalScreen();
  testWrap();
  testWrapScrolls();
  return testDone();
}
