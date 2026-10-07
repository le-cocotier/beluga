/* Windows stand-in for <termios.h> — see compat/win32/README.md.
 * There is no line discipline to configure on Windows; tcgetattr() and
 * tcsetattr() (src/platform_win32.c) map "raw" and "cooked" onto console
 * modes instead. Only ICANON is looked at: clear means raw mode (VT input
 * and output on, line editing and echo off), set means "restore the
 * console modes saved by the first tcgetattr()". */
#ifndef BELUGA_COMPAT_TERMIOS_H_
#define BELUGA_COMPAT_TERMIOS_H_

#define NCCS 32

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

#define BRKINT  0x0001
#define ICRNL   0x0002
#define INPCK   0x0004
#define ISTRIP  0x0008
#define IXON    0x0010

#define OPOST   0x0001

#define CS8     0x0030

#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define IEXTEN  0x8000

#define VTIME   5
#define VMIN    6

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);

#endif
