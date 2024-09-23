#include "../include/terminal.h"

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
  perror(s);
  exit(1);
}

void disableRawMode(struct editorConfig *E) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E->orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode(struct editorConfig *E) {
  if (tcgetattr(STDIN_FILENO, &E->orig_termios) == -1) {
    die("tcgetattr");
  }

  struct termios raw = E->orig_termios;
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

int editorReadKey() {
  int nread;
  char c;
  char seq[3];
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
        read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return BEG_LINE;
          case '3':
            return DELETE;
          case '4':
            return END_LINE;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return BEG_LINE;
          case '8':
            return END_LINE;
          }
        }
      } else {

        switch (seq[1]) {
        case 'A':
          return CURSOR_UP;
        case 'B':
          return CURSOR_DOWN;
        case 'C':
          return CURSOR_LEFT;
        case 'D':
          return CURSOR_RIGHT;
        case 'H':
          return BEG_LINE;
        case 'F':
          return END_LINE;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return BEG_LINE;
      case 'F':
        return END_LINE;
      }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }
    if (buf[i] == 'R') {
      break;
    }
    ++i;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}
