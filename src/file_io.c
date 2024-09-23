#include "../include/file_io.h"

void editorOpen(struct editorConfig *E, char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;

  while ((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 &&
           (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
      --line_len;
    }
    editorAppendRow(E, line, line_len);
  }
  free(line);
  fclose(fp);
}
