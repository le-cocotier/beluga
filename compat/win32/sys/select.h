/* Windows stand-in for <sys/select.h> — see compat/win32/README.md.
 * Windows' own select() only works on sockets. This one works on what the
 * editor actually waits on: the console (descriptor 0) and anonymous pipes
 * (language servers, background processes, terminal panes). Only the read
 * set is supported; beluga never passes the others. */
#ifndef BELUGA_COMPAT_SYS_SELECT_H_
#define BELUGA_COMPAT_SYS_SELECT_H_

#include <sys/time.h> /* struct timeval */

#define BELUGA_FD_SETSIZE 64

typedef struct {
    int count;
    int fds[BELUGA_FD_SETSIZE];
} beluga_fd_set;

#define fd_set beluga_fd_set
#ifndef FD_SETSIZE
#define FD_SETSIZE BELUGA_FD_SETSIZE
#endif

void beluga_fd_zero(beluga_fd_set *set);
void beluga_fd_add(int fd, beluga_fd_set *set);
void beluga_fd_remove(int fd, beluga_fd_set *set);
int beluga_fd_isset(int fd, const beluga_fd_set *set);

#undef FD_ZERO
#undef FD_SET
#undef FD_CLR
#undef FD_ISSET
#define FD_ZERO(set)      beluga_fd_zero(set)
#define FD_SET(fd, set)   beluga_fd_add((fd), (set))
#define FD_CLR(fd, set)   beluga_fd_remove((fd), (set))
#define FD_ISSET(fd, set) beluga_fd_isset((fd), (set))

int beluga_select(int nfds, beluga_fd_set *readfds, beluga_fd_set *writefds,
                  beluga_fd_set *exceptfds, struct timeval *timeout);
#define select(n, r, w, e, t) beluga_select((n), (r), (w), (e), (t))

#endif
