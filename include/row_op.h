#ifndef ROW_OP_H_
#define ROW_OP_H_

#include "data.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void editorAppendRow(struct editorConfig *E, char *s, size_t len);

#endif // ROW_OP_H_
