/**
 * \file main.c
 * \author Arthur Barraux
 * \brief Base file of Beluga text editor. Contain fonctions for terminal
 * interactions. \version 0.1 \date 21 septembre 2024
 */

#include "main.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/* defines */
#define CTRL_KEY(k) ((k) & 0x1f)

/* data */
struct termios orig_termios;

/* terminal */

/**
 * \fn void die(const char *s)
 * \brief Exit program and return s error message.
 * \param *s Error string
 * */
void die(const char *s) {
  wipeScreen();
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
    die("tcgetattr");
  }
  atexit(disableRawMode);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcgetattr");
  }
}

char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }
  return c;
}

/* output */

void wipeScreen() {
  /* Clear the screen */
  write(STDOUT_FILENO, "\x1b[2J", 4);
  /* Put the cursor at the top left corner */
  write(STDOUT_FILENO, "\x1b[H", 3);
}

/* input */
void editorProcessKeypress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    wipeScreen();
    exit(0);
    break;
  }
}

/* init */

int main() {

  enableRawMode();

  while (1) {
    wipeScreen();
    editorProcessKeypress();
  }
  return 0;
}
