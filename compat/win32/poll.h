/* Windows stand-in for <poll.h> — see compat/win32/README.md.
 * Same readiness rules as the select() stand-in: POLLIN only, on the
 * console or on pipes. A pipe whose writer has gone away reports POLLHUP,
 * and a read() on it then returns 0, as on Unix. */
#ifndef BELUGA_COMPAT_POLL_H_
#define BELUGA_COMPAT_POLL_H_

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

int beluga_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);
#define poll(f, n, t) beluga_poll((f), (n), (t))

#endif
