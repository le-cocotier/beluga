#ifndef FILE_IO_H_
#define FILE_IO_H_

#include "data.h"
#include "row_op.h"
#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

void editorOpen(struct editorConfig *E, char *filename);

#endif // FILE_IO_H_
