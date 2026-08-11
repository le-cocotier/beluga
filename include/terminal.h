
#ifndef TERMINAL_H_
#define TERMINAL_H_

/* includes */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "utf8.h"

/**
 * \fn void die(const char *s)
 * \brief Exit program and return s error message.
 * \param *s Error string
 * */

void die(const char *s);

void disableRawMode();

void enableRawMode();

Utf8Char* editorReadKey();

int getCursorPosition(int *rows, int *cols);

int getWindowSize(int *rows, int *cols);

char *keyToString(Utf8Char* key);

void appDebug(const char *fmt, ...);

#endif
