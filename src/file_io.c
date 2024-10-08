#include "../include/file_io.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char *editorRowsToString(struct editorConfig *E, int *buffer_len) {
  int tot_len = 0;
  int j;
  char *buf;
  char *p;

  for (j = 0; j < E->numrows; ++j) {
    tot_len += E->row[j].size + 1;
  }
  *buffer_len = tot_len;
  buf = malloc(tot_len);
  p = buf;
  for (j = 0; j < E->numrows; ++j) {
    memcpy(p, E->row[j].chars, E->row[j].size);
    p += E->row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(struct editorConfig *E, char *filename) {
  FILE *fp;

  free(E->filename);
  E->filename = strdup(filename);

  fp = fopen(filename, "r");
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

void editorSave(struct editorConfig *E) {
  int len;
  char *buf;
  int fd;
  if (E->filename == NULL) {
    return;
  }
  buf = editorRowsToString(E, &len);
  fd = open(E->filename, O_RDWR | O_CREAT, 0644);
  ftruncate(fd, len);
  write(fd, buf, len);
  close(fd);
  free(buf);
}
