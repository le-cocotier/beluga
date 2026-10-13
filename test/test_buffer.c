/**
 * @file test_buffer.c
 * @brief Unit tests for buffer editing and undo/redo (buffer.c)
 */

#include "test.h"
#include "../include/buffer.h"
#include "../include/galloc.h"

// Checks the buffer holds `expected`.
#define CHECK_TEXT(buf, expected)                                              \
  do {                                                                         \
    char *t_ = testBufferText(buf);                                            \
    CHECK_STR(t_, expected);                                                   \
    gfree(t_);                                                                 \
  } while (0)

static void testReplaceOnOneLine(void) {
  Buffer *buf = testBufferFromText("int foo = 1;\nreturn foo;");
  bufferApplyTextEdit(buf, 0, 4, 0, 7, "bar");
  CHECK_TEXT(buf, "int bar = 1;\nreturn foo;");
  bufferApplyTextEdit(buf, 1, 7, 1, 10, "bar");
  CHECK_TEXT(buf, "int bar = 1;\nreturn bar;");
  CHECK(buf->dirty != 0);
  bufferFree(buf);
}

static void testInsertLines(void) {
  Buffer *buf = testBufferFromText("first\nlast");
  bufferApplyTextEdit(buf, 0, 5, 0, 5, "\nsecond\nthird");
  CHECK_TEXT(buf, "first\nsecond\nthird\nlast");
  CHECK_INT(buf->numrows, 4);
  // The cursor ends after the inserted text.
  CHECK_INT(buf->y, 2);
  CHECK_INT(buf->x, 5);
  bufferFree(buf);
}

static void testDeleteAcrossLines(void) {
  Buffer *buf = testBufferFromText("one\ntwo\nthree\nfour");
  bufferDeleteRegion(buf, 0, 1, 2, 2);
  CHECK_TEXT(buf, "oree\nfour");
  CHECK_INT(buf->y, 0);
  CHECK_INT(buf->x, 1);
  bufferFree(buf);
}

static void testReindent(void) {
  // Deleting "\n    " joins a line and its indentation (what a formatter
  // sends to pull a line up): the region ends far left of the join column.
  Buffer *buf = testBufferFromText("if (x)\n    y();");
  bufferApplyTextEdit(buf, 0, 6, 1, 4, " ");
  CHECK_TEXT(buf, "if (x) y();");
  bufferFree(buf);
}

static void testRegionText(void) {
  Buffer *buf = testBufferFromText("caf\xc3\xa9\nau lait");
  char *t = bufferGetRegionText(buf, 0, 2, 1, 2);
  CHECK_STR(t, "f\xc3\xa9\nau");
  gfree(t);
  bufferFree(buf);
}

static void testUndoRedo(void) {
  Buffer *buf = testBufferFromText("alpha\nbeta\ngamma");
  bufferDeleteRegion(buf, 0, 2, 1, 2);
  CHECK_TEXT(buf, "alta\ngamma");
  bufferApplyTextEdit(buf, 1, 0, 1, 5, "delta\nepsilon");
  CHECK_TEXT(buf, "alta\ndelta\nepsilon");

  // Undo everything, one call per step, until there is nothing left.
  while (buf->undo_stack.count > 0)
    bufferUndo(buf);
  CHECK_TEXT(buf, "alpha\nbeta\ngamma");

  while (buf->redo_stack.count > 0)
    bufferRedo(buf);
  CHECK_TEXT(buf, "alta\ndelta\nepsilon");
  bufferFree(buf);
}

static void testFormatterEdits(void) {
  // A formatter's edit list, applied back to front as completion.c does,
  // then undone back to the original.
  Buffer *buf = testBufferFromText("int  main(){\nreturn 0;}");
  bufferApplyTextEdit(buf, 1, 9, 1, 9, "\n");
  bufferApplyTextEdit(buf, 1, 0, 1, 0, "  ");
  bufferApplyTextEdit(buf, 0, 11, 0, 11, " ");
  bufferApplyTextEdit(buf, 0, 3, 0, 5, " ");
  CHECK_TEXT(buf, "int main() {\n  return 0;\n}");
  while (buf->undo_stack.count > 0)
    bufferUndo(buf);
  CHECK_TEXT(buf, "int  main(){\nreturn 0;}");
  bufferFree(buf);
}

static void testReadOnly(void) {
  Buffer *buf = testBufferFromText("keep");
  buf->state = READ_ONLY;
  bufferApplyTextEdit(buf, 0, 0, 0, 4, "lost");
  bufferDeleteRegion(buf, 0, 0, 0, 2);
  CHECK_TEXT(buf, "keep");
  bufferFree(buf);
}

int main(void) {
  testInit();
  testReplaceOnOneLine();
  testInsertLines();
  testDeleteAcrossLines();
  testReindent();
  testRegionText();
  testUndoRedo();
  testFormatterEdits();
  testReadOnly();
  return testDone();
}
