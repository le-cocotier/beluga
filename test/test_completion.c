/**
 * @file test_completion.c
 * @brief Unit tests for TAB path completion in prompts (input.c)
 */

#include "test.h"
#include "../include/input.h"
#include "../include/os.h"

#include <stdio.h>

static char root[64];

static void touch(const char *name) {
  char path[128];
  FILE *f;
  snprintf(path, sizeof(path), "%s/%s", root, name);
  f = fopen(path, "w");
  if (f)
    fclose(f);
}

static void mkdirIn(const char *name) {
  char path[128];
  char err[128];
  snprintf(path, sizeof(path), "%s/%s", root, name);
  os_mkdirs(path, err, sizeof(err));
}

static int complete(const char *rest, FileCompletion *fc) {
  char path[128];
  snprintf(path, sizeof(path), "%s/%s", root, rest);
  return fileCompletionMatches(path, fc);
}

static void testCommonPrefix(void) {
  FileCompletion fc;
  CHECK_INT(complete("al", &fc), 2);
  CHECK_STR(fc.names[0], "alpha.txt");
  CHECK_STR(fc.names[1], "alpine/");
  CHECK_INT(fc.common, 3); // "alp"
  fileCompletionFree(&fc);
}

static void testSingleDirectoryMatch(void) {
  FileCompletion fc;
  char dir[96];
  CHECK_INT(complete("alpi", &fc), 1);
  CHECK_STR(fc.names[0], "alpine/");
  CHECK_INT(fc.common, 7);
  snprintf(dir, sizeof(dir), "%s/", root);
  CHECK_STR(fc.dir, dir);
  fileCompletionFree(&fc);
}

static void testNoMatch(void) {
  FileCompletion fc;
  CHECK_INT(complete("zzz", &fc), 0);
  fileCompletionFree(&fc);
}

static void testHiddenOnlyOnDot(void) {
  FileCompletion fc;
  CHECK_INT(complete("", &fc), 6); // all but .hidden
  fileCompletionFree(&fc);
  CHECK_INT(complete(".", &fc), 1);
  CHECK_STR(fc.names[0], ".hidden");
  fileCompletionFree(&fc);
}

static void testUtf8Boundary(void) {
  // "\xc3\xa9" and "\xc3\xa8" share their first byte, which is not a
  // character by itself: the common prefix stops before it.
  FileCompletion fc;
  CHECK_INT(complete("\xc3", &fc), 2);
  CHECK_INT(fc.common, 0);
  fileCompletionFree(&fc);
}

int main(void) {
  char err[128];
  testInit();
  snprintf(root, sizeof(root), "completion-test-%ld", os_getpid());
  mkdirIn("");
  touch("alpha.txt");
  mkdirIn("alpine");
  touch("beta");
  touch(".hidden");
  touch("\xc3\xa9t\xc3\xa9");
  touch("\xc3\xa8re");
  mkdirIn("zeta");
  testCommonPrefix();
  testSingleDirectoryMatch();
  testNoMatch();
  testHiddenOnlyOnDot();
  testUtf8Boundary();
  os_delete_tree(root, err, sizeof(err));
  return testDone();
}
