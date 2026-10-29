/**
 * @file test_breakpoints.c
 * @brief Unit tests for the breakpoint list and how it follows edits
 * (breakpoints.c, buffer.c)
 */

#include "test.h"
#include "../include/breakpoints.h"
#include "../include/buffer.h"
#include "../include/galloc.h"
#include "../include/os.h"
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
  CHECK_INT(breakpointSetHitCondition("/src/main.c", 6, "% 2"), 0); // adds one
  CHECK_STR(breakpointAt("/src/main.c", 6)->hit_condition, "% 2");
  CHECK_INT(breakpointSetLogMessage("/src/main.c", 6, "at {i}"), 0);
  CHECK_STR(breakpointAt("/src/main.c", 6)->log_message, "at {i}");
  CHECK_INT(breakpointSetLogMessage("/src/main.c", 6, NULL), 0);
  CHECK(breakpointAt("/src/main.c", 6)->log_message == NULL);
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

static void testEnterBeforeTheCode(void) {
  Buffer *buf = fileBuffer("a\n  b\nc");
  breakpointToggle("/src/main.c", 0); // "a"
  breakpointToggle("/src/main.c", 1); // "  b"
  // ENTER at the start of "a", and after the indent of "  b": both
  // breakpoints go down with their code.
  bufferApplyTextEdit(buf, 0, 0, 0, 0, "\n");
  CHECK(breakpointAt("/src/main.c", 0) == NULL);
  CHECK(breakpointAt("/src/main.c", 1) != NULL); // "a"
  CHECK(breakpointAt("/src/main.c", 2) != NULL); // "  b"
  bufferApplyTextEdit(buf, 2, 2, 2, 2, "\n\n");
  CHECK(breakpointAt("/src/main.c", 2) == NULL);
  CHECK(breakpointAt("/src/main.c", 4) != NULL);
  // Undo takes the two lines out again: the breakpoint goes back up.
  bufferUndo(buf);
  CHECK(breakpointAt("/src/main.c", 2) != NULL);
  CHECK(breakpointAt("/src/main.c", 4) == NULL);
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

static void testKeptAcrossRuns(void) {
  const char *file = "test_breakpoints.store";
  char self[OS_PATH_MAX];
  char *cwd = os_getcwd();
  FILE *f;
  snprintf(self, sizeof(self), "%s/%s", cwd, file);
  gfree(cwd);
  f = fopen(self, "w"); // a file that exists, for the breakpoints to be in
  CHECK(f != NULL);
  fprintf(f, "x\n");
  fclose(f);
  breakpointToggle(self, 3);
  breakpointSetCondition(self, 7, "i\t== 2");
  breakpointSetHitCondition(self, 7, "3");
  breakpointSetLogMessage(self, 5, "i = {i}");
  breakpointToggle("/no/such/file.c", 1);
  CHECK_INT(breakpointsSave(file), 0);
  breakpointsClear();
  // The gone file's breakpoint isn't brought back; the others are, with
  // the condition (its tab now a space), and loading tells nobody.
  s_notified = 0;
  CHECK_INT(breakpointsLoad(file), 3);
  CHECK_INT(s_notified, 0);
  CHECK(breakpointAt(self, 3) != NULL);
  CHECK(breakpointAt(self, 3)->log_message == NULL);
  CHECK_STR(breakpointAt(self, 7)->condition, "i == 2");
  CHECK_STR(breakpointAt(self, 7)->hit_condition, "3");
  CHECK_STR(breakpointAt(self, 5)->log_message, "i = {i}");
  CHECK(breakpointAt(self, 5)->condition == NULL);
  CHECK(breakpointAt("/no/such/file.c", 1) == NULL);
  // Setting a store loads it (the clear above, without one, saved
  // nothing), then every change is written at once. A line from before
  // hit counts and logpoints (three fields) still loads.
  f = fopen(file, "a");
  CHECK(f != NULL);
  fprintf(f, "12\t%s\tx > 1\n", self);
  fclose(f);
  breakpointsClear();
  breakpointsSetStore(file);
  CHECK_INT(breakpointsInFile(self), 4);
  CHECK_STR(breakpointAt(self, 11)->condition, "x > 1");
  CHECK(breakpointAt(self, 11)->hit_condition == NULL);
  breakpointToggle(self, 9);
  breakpointsSetStore(NULL);
  breakpointsClear();
  CHECK_INT(breakpointsLoad(file), 5);
  CHECK(breakpointAt(self, 9) != NULL);
  breakpointsClear();
  remove(file);
  remove(self);
}

int main(void) {
  testInit();
  breakpointsSetListener(onChange);
  testToggle();
  testCondition();
  testFollowsInsertedLines();
  testEnterBeforeTheCode();
  testFollowsRemovedLines();
  testOtherFilesDontMove();
  testKeptAcrossRuns();
  breakpointsFree();
  return testDone();
}
