/**
 * @file test_utils.c
 * @brief Unit tests for the string and path helpers in utils.c, and the
 * append buffer the renderer builds frames in (append_buffer.c).
 */

#include "test.h"
#include "../include/append_buffer.h"
#include "../include/galloc.h"
#include "../include/utils.h"

static void testStrdup(void) {
  char *s;

  s = utilStrdup("hello");
  CHECK_STR(s, "hello");
  gfree(s);

  s = utilStrdup("");
  CHECK_STR(s, "");
  gfree(s);

  s = utilStrndup("hello", 3);
  CHECK_STR(s, "hel");
  gfree(s);

  // n past the end of the string copies the string, not n bytes.
  s = utilStrndup("hi", 10);
  CHECK_STR(s, "hi");
  gfree(s);

  s = utilStrndup("hello", 0);
  CHECK_STR(s, "");
  gfree(s);
}

static void testStrtok(void) {
  char text[] = "  one,two,,three  ";
  char empty[] = ",,, ";
  char *save = NULL;

  CHECK_STR(utilStrtok(text, " ,", &save), "one");
  CHECK_STR(utilStrtok(NULL, " ,", &save), "two");
  CHECK_STR(utilStrtok(NULL, " ,", &save), "three");
  CHECK(utilStrtok(NULL, " ,", &save) == NULL);
  // Asking again after the end keeps answering NULL.
  CHECK(utilStrtok(NULL, " ,", &save) == NULL);

  save = NULL;
  CHECK(utilStrtok(empty, " ,", &save) == NULL);
}

static void testMemmem(void) {
  const char hay[] = "abc\0def\r\n\r\nxyz";
  const size_t len = sizeof(hay) - 1;

  // Finds a needle past an embedded NUL, which strstr() can't.
  CHECK(utilMemmem(hay, len, "def", 3) == hay + 4);
  CHECK(utilMemmem(hay, len, "\r\n\r\n", 4) == hay + 7);
  CHECK(utilMemmem(hay, len, "xyz", 3) == hay + 11);
  CHECK(utilMemmem(hay, len, "xyzw", 4) == NULL);
  CHECK(utilMemmem(hay, len, "q", 1) == NULL);
  // Only the first `haystack_len` bytes are searched.
  CHECK(utilMemmem(hay, 6, "def", 3) == NULL);
  CHECK(utilMemmem(hay, len, "", 0) == hay);
  CHECK(utilMemmem(hay, 0, "a", 1) == NULL);
}

static void testFormat(void) {
  char *s;
  char big[2000];

  s = utilFormat("%s-%d", "x", 42);
  CHECK_STR(s, "x-42");
  gfree(s);

  s = utilFormat("%s", "");
  CHECK_STR(s, "");
  gfree(s);

  // No fixed-size buffer: long results come back whole.
  memset(big, 'a', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  s = utilFormat("<%s>", big);
  CHECK_INT(strlen(s), sizeof(big) + 1);
  CHECK(s[0] == '<' && s[sizeof(big)] == '>');
  gfree(s);
}

static void checkDirname(const char *path, const char *expected) {
  char out[256];
  utilDirname(path, out, sizeof(out));
  testCheckStr(__FILE__, __LINE__, path, out, expected);
}

static void testDirname(void) {
  checkDirname("/usr/lib/file.c", "/usr/lib");
  checkDirname("/usr/lib/", "/usr");
  checkDirname("/usr//lib", "/usr");
  checkDirname("dir/file", "dir");
  checkDirname("file", ".");
  checkDirname("/file", "/");
  checkDirname("/", "/");
  checkDirname("", ".");
}

static void testPathIsUnder(void) {
  CHECK(utilPathIsUnder("/home/me/src/a.c", "/home/me"));
  CHECK(utilPathIsUnder("/home/me/src/a.c", "/home/me/"));
  CHECK(utilPathIsUnder("/home/me", "/home/me"));
  CHECK(utilPathIsUnder("/anything", "/"));
  // A sibling sharing the prefix isn't inside.
  CHECK(!utilPathIsUnder("/home/meow/a.c", "/home/me"));
  CHECK(!utilPathIsUnder("/home", "/home/me"));
  CHECK(!utilPathIsUnder(NULL, "/home"));
  CHECK(!utilPathIsUnder("/home", NULL));
}

static void testRecalloc(void) {
  unsigned char *p;
  int i;
  int zeros;

  p = recalloc(NULL, 0, 8);
  CHECK(p != NULL);
  memset(p, 0xAB, 8);
  p = recalloc(p, 8, 64);
  CHECK(p != NULL);
  // The old bytes are kept, the new ones zeroed.
  CHECK(p[0] == 0xAB && p[7] == 0xAB);
  zeros = 0;
  for (i = 8; i < 64; i++)
    zeros += p[i] == 0;
  CHECK_INT(zeros, 56);
  // Shrinking keeps the front.
  p = recalloc(p, 64, 4);
  CHECK(p != NULL && p[3] == 0xAB);
  gfree(p);
}

static void testAppendBuffer(void) {
  struct abuf ab = {NULL, 0, 0};
  int i;

  abAppend(&ab, "abc", 3);
  abAppend(&ab, "", 0);
  abAppend(&ab, "defgh", 2); // only `len` bytes
  CHECK_INT(ab.len, 5);
  CHECK(memcmp(ab.b, "abcde", 5) == 0);

  for (i = 0; i < 10000; i++)
    abAppend(&ab, "x", 1);
  CHECK_INT(ab.len, 10005);
  CHECK(ab.b[10004] == 'x' && ab.b[4] == 'e');

  abFree(&ab);
}

int main(void) {
  testInit();
  testStrdup();
  testStrtok();
  testMemmem();
  testFormat();
  testDirname();
  testPathIsUnder();
  testRecalloc();
  testAppendBuffer();
  return testDone();
}
