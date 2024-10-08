#ifndef FILE_IO_H_
#define FILE_IO_H_

#include "data.h"
#include "row_op.h"
#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

char *editorRowsToString(struct editorConfig *E, int *buffer_len);

void editorOpen(struct editorConfig *E, char *filename);

void editorSave(struct editorConfig *E);

#endif // FILE_IO_H_
