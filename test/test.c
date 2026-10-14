/**
 * @file test.c
 * @brief Shared helpers for the unit tests (see test.h)
 * @details Defines the globals main.c normally owns, so a test links the
 * editor's code without main().
 */

#include "test.h"
#include "../include/buffer.h"
#include "../include/galloc.h"
#include "../include/utf8.h"

#include <stdlib.h>

EditorConfig E;
int buffer_id = 0;
int test_failures = 0;

void testCheckStr(const char *file, int line, const char *what,
                  const char *actual, const char *expected) {
  if (!actual || strcmp(actual, expected) != 0) {
    fprintf(stderr, "%s:%d: %s is \"%s\", expected \"%s\"\n", file, line, what,
            actual ? actual : "(null)", expected);
    test_failures++;
  }
}

void testInit(void) {
  E.status_msg = gcalloc(PATH_MAX, sizeof(Utf8Char));
}

int testDone(void) {
  if (test_failures)
    fprintf(stderr, "%d check(s) failed\n", test_failures);
  return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

Buffer *testBufferFromText(const char *text) {
  Buffer *buf;
  const char *line;
  const char *end;
  Utf8Char chars[1024];
  int n;
  const unsigned char *p;

  buf = gcalloc(1, sizeof(Buffer));
  buf->type = FILE_BUFF;
  buf->state = READ_AND_WRITE;
  line = text;
  for (;;) {
    end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);
    n = 0;
    for (p = (const unsigned char *)line; p < (const unsigned char *)end && n < 1023;)
      p += utf8CreateChar(p, &chars[n++]);
    bufferInsertRow(buf, buf->numrows, chars, n);
    if (!*end)
      break;
    line = end + 1;
  }
  buf->dirty = 0;
  return buf;
}

char *testBufferText(Buffer *buf) {
  char *text;
  size_t len;
  text = bufferToText(buf);
  len = text ? strlen(text) : 0;
  if (len > 0 && text[len - 1] == '\n')
    text[len - 1] = '\0'; // bufferToText() ends every row with one
  return text;
}
