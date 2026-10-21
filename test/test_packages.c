/**
 * @file test_packages.c
 * @brief Unit tests for packages.c's version comparison (the rest of the
 * package manager is tested end to end by test_packages.sh)
 */

#include "test.h"
#include "../include/packages.h"

static int sign(const int n) { return n < 0 ? -1 : n > 0 ? 1 : 0; }

static void testVersionCompare(void) {
  CHECK_INT(sign(packageVersionCompare("1.0", "1.0")), 0);
  CHECK_INT(sign(packageVersionCompare("1.10", "1.9")), 1);
  CHECK_INT(sign(packageVersionCompare("1.9", "1.10")), -1);
  CHECK_INT(sign(packageVersionCompare("2", "2.0.0")), 0);
  CHECK_INT(sign(packageVersionCompare("2.0.1", "2")), 1);
  CHECK_INT(sign(packageVersionCompare("v1.2.0", "1.2")), 0);
  CHECK_INT(sign(packageVersionCompare("V3", "v2.9")), 1);
  CHECK_INT(sign(packageVersionCompare("1.2.0-rc1", "1.2.0")), 0);
  CHECK_INT(sign(packageVersionCompare("3.3", "3.4")), -1);
  CHECK_INT(sign(packageVersionCompare("", "0")), 0);
  CHECK_INT(sign(packageVersionCompare("bundled", "1.0")), -1);
}

int main(void) {
  testInit();
  testVersionCompare();
  return testDone();
}
