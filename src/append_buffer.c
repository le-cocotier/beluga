#include "../include/append_buffer.h"

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = NULL;
  new = (char *)realloc(ab->b, (ab->len + len + 1) * sizeof(char));

  if (new == NULL) {
    return;
  }
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
  ab->b = NULL;
  ab->len = 0;
}
