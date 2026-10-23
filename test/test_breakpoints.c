/**
 * @file test_breakpoints.c
 * @brief Unit tests for the breakpoint list and how it follows edits
 * (breakpoints.c, buffer.c)
 */

#include "test.h"
#include "../include/breakpoints.h"
#include "../include/buffer.h"
#include "../include/galloc.h"
#include "../include/utf8.h"

static int s_notified;
static char s_notified_path[64];

static void onChange(const char *path) {
  s_notified++;
  snprintf(s_notified_path, sizeof(s_notified_path), "%s", path);
}

static Buffer *fileBuffer(const char *text) {
  Buffer *buf = testBufferFromText(text);
  buf->fullname = stringToUtf8((unsigned char *)"/src/main.c");
  return buf;
}

static void testToggle(void) {
  int count;
  CHECK_INT(breakpointToggle("/src/main.c", 4), 1);
  CHECK_INT(breakpointToggle("/src/a.c", 9), 1);
  CHECK_INT(breakpointToggle("/src/main.c", 1), 1);
  CHECK_INT(breakpointsInFile("/src/main.c"), 2);
  // Sorted by path, then line.
  breakpointsAll(&count);
  CHECK_INT(count, 3);
  CHECK_STR(breakpointsAll(&count)[0].path, "/src/a.c");
  CHECK_INT(breakpointsAll(&count)[1].line, 1);
  CHECK_INT(breakpointToggle("/src/main.c", 4), 0);
  CHECK(breakpointAt("/src/main.c", 4) == NULL);
  CHECK(breakpointAt("/src/main.c", 1) != NULL);
  CHECK_STR(s_notified_path, "/src/main.c");
  CHECK_INT(breakpointsClear(), 2);
  CHECK_INT(breakpointsInFile("/src/main.c"), 0);
}

static void testCondition(void) {
  CHECK_INT(breakpointSetCondition("/src/main.c", 2, "i == 3"), 0);
  CHECK_STR(breakpointAt("/src/main.c", 2)->condition, "i == 3");
  CHECK_INT(breakpointSetCondition("/src/main.c", 2, ""), 0);
  CHECK(breakpointAt("/src/main.c", 2)->condition == NULL);
  breakpointsClear();
}

static void testFollowsInsertedLines(void) {
  Buffer *buf = fileBuffer("a\nb\nc\nd");
  breakpointToggle("/src/main.c", 2); // "c"
  breakpointToggle("/src/main.c", 0); // "a"
  s_notified = 0;
  bufferApplyTextEdit(buf, 1, 1, 1, 1, "\nnew\nlines"); // after "b"
  CHECK(breakpointAt("/src/main.c", 4) != NULL);
  CHECK(breakpointAt("/src/main.c", 2) == NULL);
  CHECK(breakpointAt("/src/main.c", 0) != NULL);
  CHECK(s_notified > 0);
  // ENTER in the middle of "a": the breakpoint stays on its line.
  bufferApplyTextEdit(buf, 0, 1, 0, 1, "\n");
  CHECK(breakpointAt("/src/main.c", 0) != NULL);
  CHECK(breakpointAt("/src/main.c", 5) != NULL);
  breakpointsClear();
  bufferFree(buf);
}

static void testFollowsRemovedLines(void) {
  Buffer *buf = fileBuffer("a\nb\nc\nd\ne");
  breakpointToggle("/src/main.c", 1); // "b"
  breakpointToggle("/src/main.c", 2); // "c"
  breakpointToggle("/src/main.c", 4); // "e"
  // Join "b", "c" and "d" into one line: the two on it become one.
  bufferDeleteRegion(buf, 1, 1, 3, 0);
  CHECK_INT(breakpointsInFile("/src/main.c"), 2);
  CHECK(breakpointAt("/src/main.c", 1) != NULL);
  CHECK(breakpointAt("/src/main.c", 2) != NULL); // "e"
  // Undo puts the rows back; "e" moves down with them.
  bufferUndo(buf);
  CHECK(breakpointAt("/src/main.c", 4) != NULL);
  breakpointsClear();
  bufferFree(buf);
}

static void testOtherFilesDontMove(void) {
  Buffer *buf = fileBuffer("a\nb\nc");
  breakpointToggle("/src/other.c", 1);
  bufferApplyTextEdit(buf, 0, 0, 0, 0, "x\ny\n");
  CHECK(breakpointAt("/src/other.c", 1) != NULL);
  breakpointsClear();
  bufferFree(buf);
}

int main(void) {
  testInit();
  breakpointsSetListener(onChange);
  testToggle();
  testCondition();
  testFollowsInsertedLines();
  testFollowsRemovedLines();
  testOtherFilesDontMove();
  breakpointsFree();
  return testDone();
}
