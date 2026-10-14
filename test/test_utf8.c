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

static void testInvalid(void) {
  Utf8Char c;
  // Cut short by the terminator: one byte, never reading past it.
  CHECK_INT(utf8CreateChar((const unsigned char *)"\xf0\x9f", &c), 1);
  CHECK_INT(c.length, 1);
  CHECK_INT(c.character[0], 0xf0);
  // A lead byte followed by ASCII keeps the ASCII for the next character.
  CHECK_INT(utf8CreateChar((const unsigned char *)"\xc3" "A", &c), 1);
  // A stray continuation byte.
  CHECK_INT(utf8CreateChar((const unsigned char *)"\x80x", &c), 1);
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

static void testWidth(void) {
  // "a", "e" + combining acute, CJK "中", emoji "😀", "z"
  Utf8Char *u = stringToUtf8((unsigned char *)"ae\xcc\x81\xe4\xb8\xad\xf0\x9f\x98\x80z");
  int n = utf8StringLen(u) - 1;
  Utf8Span span;
  CHECK_INT(n, 6);
  CHECK_INT(utf8CharWidth(&u[0]), 1);
  CHECK_INT(utf8CharWidth(&u[2]), 0);
  CHECK_INT(utf8CharWidth(&u[3]), 2);
  CHECK_INT(utf8CharWidth(&u[4]), 2);
  CHECK_INT(utf8ColumnOf(u, n, 3), 2);
  CHECK_INT(utf8ColumnOf(u, n, 5), 6);
  CHECK_INT(utf8ColumnOf(u, n, 6), 7);
  CHECK_INT(utf8IndexAtColumn(u, n, 1), 1);
  CHECK_INT(utf8IndexAtColumn(u, n, 3), 3); // right half of the CJK char
  CHECK_INT(utf8IndexAtColumn(u, n, 6), 5);
  CHECK_INT(utf8IndexAtColumn(u, n, 9), 6);

  // Scrolled to column 3: the CJK char is cut, a blank stands in for it.
  utf8VisibleSpan(u, n, 3, 10, &span);
  CHECK_INT(span.lead_pad, 1);
  CHECK_INT(span.start, 4);
  CHECK_INT(span.end, 6);
  CHECK_INT(span.columns, 4);

  // 4 columns from 0: "a", "e\u0301", "中"; the emoji doesn't fit.
  utf8VisibleSpan(u, n, 0, 4, &span);
  CHECK_INT(span.start, 0);
  CHECK_INT(span.end, 4);
  CHECK_INT(span.columns, 4);
  utf8VisibleSpan(u, n, 0, 5, &span);
  CHECK_INT(span.end, 4);
  CHECK_INT(span.columns, 4);
  gfree(u);
}

int main(void) {
  testInit();
  testCreateChar();
  testInvalid();
  testRoundTrip();
  testCompare();
  testExtension();
  testPrintf();
  testWidth();
  return testDone();
}
