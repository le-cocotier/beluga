#ifndef BELUGA_TEST_H_
#define BELUGA_TEST_H_

// A minimal test harness: CHECK* record a failure and keep going, and
// testDone() turns the count into the exit status meson test reads.

#include "../include/data.h"

#include <stdio.h>
#include <string.h>

extern int test_failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      test_failures++;                                                         \
    }                                                                          \
  } while (0)

#define CHECK_INT(actual, expected)                                            \
  do {                                                                         \
    long a_ = (long)(actual);                                                  \
    long e_ = (long)(expected);                                                \
    if (a_ != e_) {                                                            \
      fprintf(stderr, "%s:%d: %s is %ld, expected %ld\n", __FILE__, __LINE__,  \
              #actual, a_, e_);                                                \
      test_failures++;                                                         \
    }                                                                          \
  } while (0)

#define CHECK_STR(actual, expected)                                            \
  testCheckStr(__FILE__, __LINE__, #actual, (actual), (expected))

void testCheckStr(const char *file, int line, const char *what,
                  const char *actual, const char *expected);

// Sets up the parts of the global editor state the code under test
// touches (the status message buffer).
void testInit(void);
int testDone(void);

// A file buffer holding `text` ('\n' between rows, no trailing newline).
Buffer *testBufferFromText(const char *text);
// The buffer's text in the same form (caller frees with gfree).
char *testBufferText(Buffer *buf);

#endif // BELUGA_TEST_H_
