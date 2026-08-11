#ifndef FILE_IO_H_
#define FILE_IO_H_

#include "data.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

void editorCloseFile(void);

void editorOpen(struct buffer_t *buffer);

void editorSave();

#endif // FILE_IO_H_
