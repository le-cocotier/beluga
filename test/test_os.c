/**
 * @file test_os.c
 * @brief Unit tests for the OS layer (os.h), run natively and, for
 * os_win32.c, under Wine
 * @details Works in a scratch directory under the current one (meson
 * runs tests in the build directory). Child processes are this program
 * itself, started with --child, so the test needs nothing installed.
 */

#include "test.h"
#include "../include/galloc.h"
#include "../include/os.h"

#include <stdlib.h>

#define SCRATCH "os-test-scratch"

static int writeFile(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  fputs(text, f);
  fclose(f);
  return 1;
}

static int isWindows(void) { return strcmp(os_name(), "windows") == 0; }

static void testFiles(void) {
  char err[256];
  OsFileInfo info;
  OsDirEntry *entries;
  int count;
  int i;
  int saw_file;
  int saw_dir;
  const char *sources[1];

  os_delete_tree(SCRATCH, err, sizeof(err));
  CHECK_INT(os_mkdirs(SCRATCH "/a/b", err, sizeof(err)), 0);
  CHECK_INT(os_file_info(SCRATCH "/a/b", &info), 0);
  CHECK(info.is_dir);

  CHECK(writeFile(SCRATCH "/a/hello.txt", "hello\n"));
  CHECK_INT(os_file_info(SCRATCH "/a/hello.txt", &info), 0);
  CHECK(!info.is_dir);
  CHECK_INT(info.size, 6);
  CHECK_INT(os_file_info(SCRATCH "/a/missing", &info), -1);

  entries = os_list_dir(SCRATCH "/a", &count);
  CHECK(entries != NULL);
  CHECK_INT(count, 2);
  saw_file = saw_dir = 0;
  for (i = 0; entries && i < count; i++) {
    if (strcmp(entries[i].name, "hello.txt") == 0 && !entries[i].is_dir)
      saw_file = 1;
    if (strcmp(entries[i].name, "b") == 0 && entries[i].is_dir)
      saw_dir = 1;
  }
  CHECK(saw_file && saw_dir);
  os_free_dir(entries, count);

  sources[0] = SCRATCH "/a";
  CHECK_INT(os_copy_tree(sources, 1, SCRATCH "/copy", err, sizeof(err)), 0);
  CHECK_INT(os_file_info(SCRATCH "/copy/hello.txt", &info), 0);
  CHECK_INT(info.size, 6);

  sources[0] = SCRATCH "/copy/hello.txt";
  CHECK_INT(os_move(sources, 1, SCRATCH "/moved.txt", err, sizeof(err)), 0);
  CHECK_INT(os_file_info(SCRATCH "/copy/hello.txt", &info), -1);
  CHECK_INT(os_file_info(SCRATCH "/moved.txt", &info), 0);

  CHECK_INT(os_delete_tree(SCRATCH, err, sizeof(err)), 0);
  CHECK_INT(os_file_info(SCRATCH, &info), -1);
}

static void testPaths(void) {
  char path[64];
  CHECK(os_path_is_absolute(isWindows() ? "C:\\x" : "/x"));
  CHECK(!os_path_is_absolute("x/y"));
  CHECK(os_path_equal("dir/file", "dir/file"));
  CHECK(!os_path_equal("dir/file", "dir/other"));
  // File names are case-insensitive on Windows only.
  CHECK_INT(os_path_equal("dir/File", "dir/file"), isWindows());
  strcpy(path, "a\\b");
  os_path_to_slashes(path);
  CHECK_STR(path, isWindows() ? "a/b" : "a\\b");
  CHECK(os_home_dir() != NULL);
}

static void testProcess(const char *self) {
  const char *argv[3];
  OsSpawn spawn;
  OsProc *proc;
  char out[256];
  long n;
  long total;
  int exit_code;
  long long start;

  argv[0] = self;
  argv[1] = "--child";
  argv[2] = NULL;
  memset(&spawn, 0, sizeof(spawn));
  spawn.argv = argv;
  spawn.stderr_mode = OS_STDERR_MERGE;
  proc = os_proc_spawn(&spawn);
  CHECK(proc != NULL);
  if (!proc)
    return;

  total = 0;
  start = os_monotonic_ms();
  while (total < (long)sizeof(out) - 1 && os_monotonic_ms() - start < 20000) {
    n = os_stream_read(os_proc_output(proc), out + total,
                       sizeof(out) - 1 - (size_t)total, 1000);
    if (n < 0)
      break;
    total += n;
  }
  out[total] = '\0';
  CHECK(strstr(out, "hello from the child") != NULL);
  CHECK_INT(os_proc_wait(proc, 20000, &exit_code), 1);
  CHECK_INT(exit_code, 3);
  os_proc_free(proc);
}

static void testMissingProgram(void) {
  const char *argv[2];
  OsSpawn spawn;
  OsProc *proc;
  int exit_code;
  argv[0] = "beluga-no-such-program";
  argv[1] = NULL;
  memset(&spawn, 0, sizeof(spawn));
  spawn.argv = argv;
  proc = os_proc_spawn(&spawn);
  CHECK(proc != NULL);
  if (!proc)
    return;
  CHECK_INT(os_proc_wait(proc, 20000, &exit_code), 1);
  CHECK_INT(exit_code, 127);
  os_proc_free(proc);
}

static void testFindProgram(const char *self) {
  char *found = os_find_program(isWindows() ? "cmd" : "sh");
  CHECK(found != NULL);
  CHECK(found && os_path_is_absolute(found));
  gfree(found);
  CHECK(os_find_program("beluga-no-such-program") == NULL);
  CHECK(os_find_program("") == NULL);
  // A path is checked as given: this test program is there, nothing else.
  found = os_find_program(self);
  CHECK(found != NULL);
  gfree(found);
  CHECK(os_find_program("./beluga-no-such-program") == NULL);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--child") == 0) {
    printf("hello from the child\n");
    fflush(stdout);
    return 3;
  }
  os_init();
  testInit();
  testFiles();
  testPaths();
  testProcess(argv[0]);
  testMissingProgram();
  testFindProgram(argv[0]);
  return testDone();
}
