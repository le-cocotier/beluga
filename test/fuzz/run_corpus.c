/**
 * @file run_corpus.c
 * @brief main() for a fuzz target built without libFuzzer: runs it once
 * over every file in the corpus directories given as arguments, so the
 * seeds (and any crash reproducers saved there) are regression tests.
 */

#include "../test.h"
#include "../../include/galloc.h"
#include "../../include/os.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int runFile(const char *path) {
  FILE *f;
  unsigned char *data;
  long size;
  f = fopen(path, "rb");
  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  data = galloc(size > 0 ? (size_t)size : 1);
  if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
    gfree(data);
    fclose(f);
    return 0;
  }
  fclose(f);
  LLVMFuzzerTestOneInput(data, (size_t)size);
  gfree(data);
  return 1;
}

int main(int argc, char **argv) {
  OsDirEntry *entries;
  int count;
  int i;
  int j;
  int ran;
  char path[OS_PATH_MAX];
  ran = 0;
  for (i = 1; i < argc; i++) {
    entries = os_list_dir(argv[i], &count);
    for (j = 0; entries && j < count; j++) {
      if (entries[j].is_dir)
        continue;
      snprintf(path, sizeof(path), "%s/%s", argv[i], entries[j].name);
      ran += runFile(path);
    }
    os_free_dir(entries, count);
  }
  printf("ran %d input(s)\n", ran);
  return ran > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
