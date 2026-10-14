/**
 * @file test_utf8.c
 * @brief Unit tests for utf8.c
 */

#include "test.h"
#include "../include/galloc.h"
#include "../include/utf8.h"

static void testCreateChar(void) {
  Utf8Char c;
  CHECK_INT(utf8CreateChar((const unsigned char *)"a", &c), 1);
  CHECK_INT(c.length, 1);
  CHECK_INT(c.character[0], 'a');
  CHECK_INT(utf8CreateChar((const unsigned char *)"\xc3\xa9", &c), 2); // é
  CHECK_INT(c.length, 2);
  CHECK_INT(utf8CreateChar((const unsigned char *)"\xe2\x82\xac", &c), 3); // €
  CHECK_INT(c.length, 3);
  CHECK_INT(utf8CreateChar((const unsigned char *)"\xf0\x9f\x98\x80", &c), 4); // 😀
  CHECK_INT(c.length, 4);
  CHECK(memcmp(c.character, "\xf0\x9f\x98\x80", 4) == 0);
}

static void testRoundTrip(void) {
  const char *text = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80!";
  Utf8Char *u;
  unsigned char *back;
  u = stringToUtf8((unsigned char *)text);
  CHECK(u != NULL);
  // utf8StringLen() counts the terminator.
  CHECK_INT(utf8StringLen(u), 10);
  back = utf8ToString(u);
  CHECK_STR((const char *)back, text);
  gfree(back);
  back = utf8ToNString(u, 4);
  CHECK_STR((const char *)back, "caf\xc3\xa9");
  gfree(back);
  gfree(u);
}

static void testCompare(void) {
  Utf8Char *a;
  Utf8Char *b;
  a = stringToUtf8((unsigned char *)"abc\xc3\xa9");
  b = stringToUtf8((unsigned char *)"abc\xc3\xa9");
  CHECK(utf8StringCmp(a, b) == 0);
  gfree(b);
  b = stringToUtf8((unsigned char *)"abd");
  CHECK(utf8StringCmp(a, b) != 0);
  CHECK(utf8StringNCmp(a, b, 2) == 0);
  gfree(a);
  gfree(b);
}

static void testExtension(void) {
  Utf8Char *name;
  char ext[16];
  name = stringToUtf8((unsigned char *)"src/buffer.c");
  CHECK_INT(utf8FilenameExtension(name, ext, sizeof(ext)), 1);
  CHECK_STR(ext, "c");
  gfree(name);
  name = stringToUtf8((unsigned char *)"archive.tar.gz");
  utf8FilenameExtension(name, ext, sizeof(ext));
  CHECK_STR(ext, "gz");
  gfree(name);
  name = stringToUtf8((unsigned char *)"Makefile");
  CHECK_INT(utf8FilenameExtension(name, ext, sizeof(ext)), 0);
  CHECK_STR(ext, "");
  gfree(name);
}

static void testPrintf(void) {
  Utf8Char out[64];
  unsigned char *s;
  utf8Printf(out, 64, "%s/%d", "caf\xc3\xa9", 42);
  s = utf8ToString(out);
  CHECK_STR((const char *)s, "caf\xc3\xa9/42");
  gfree(s);
}

int main(void) {
  testInit();
  testCreateChar();
  testRoundTrip();
  testCompare();
  testExtension();
  testPrintf();
  return testDone();
}
