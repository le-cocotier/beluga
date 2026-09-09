#include "../include/append_buffer.h"
#include <stdbool.h>

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = NULL;
  bool need_to_resize = false;
  int old_size = ab->bf_size;
  // Check for buffer length
  while (ab->bf_size < ab->len + len) {
      ab->bf_size = ab->bf_size ? ab->bf_size * 2 : 16;
      need_to_resize = true;
  }
  if (need_to_resize) {
      new = (char *)realloc(ab->b, (ab->bf_size) * sizeof(char));
      if (new == NULL) {
          ab->bf_size = old_size;
        return;
      }
      ab->b = new;
  }
  memcpy(&ab->b[ab->len], s, len);
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
  ab->b = NULL;
  ab->len = 0;
  ab->bf_size = 0;
}
