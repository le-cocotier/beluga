
#ifndef TERMINAL_H_
#define TERMINAL_H_

/* includes */

#include "data.h"
#include "define.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/**
 * \fn void die(const char *s)
 * \brief Exit program and return s error message.
 * \param *s Error string
 * */

void die(const char *s);

void disableRawMode(struct editorConfig *E);

void enableRawMode(struct editorConfig *E);

int editorReadKey();

int getCursorPosition(int *rows, int *cols);

int getWindowSize(int *rows, int *cols);

#endif
