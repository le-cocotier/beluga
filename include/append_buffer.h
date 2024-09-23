#ifndef APPEND_BUFFER_H_
#define APPEND_BUFFER_H_

#include "data.h"
#include <stdlib.h>
#include <string.h>

void abAppend(struct abuf *ab, const char *s, int len);

void abFree(struct abuf *ab);

#endif // APPEND_BUFFER_H_
