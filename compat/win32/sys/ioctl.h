/* Windows stand-in for <sys/ioctl.h> — see compat/win32/README.md.
 * ioctl() (src/platform_win32.c) handles the two requests beluga makes:
 * TIOCGWINSZ on the console (its visible window size) and TIOCSWINSZ on a
 * terminal pane's descriptor (ResizePseudoConsole). */
#ifndef BELUGA_COMPAT_SYS_IOCTL_H_
#define BELUGA_COMPAT_SYS_IOCTL_H_

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int beluga_ioctl(int fd, unsigned long request, ...);
#define ioctl(...) beluga_ioctl(__VA_ARGS__)

#endif
