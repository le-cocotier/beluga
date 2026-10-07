/* Windows stand-in for <sys/wait.h> — see compat/win32/README.md.
 * waitpid() (src/platform_win32.c) works on the processes beluga itself
 * starts through belugaSpawn()/belugaPtySpawn(). A process ended by kill()
 * reports as signalled (not WIFEXITED), like one killed on Unix. */
#ifndef BELUGA_COMPAT_SYS_WAIT_H_
#define BELUGA_COMPAT_SYS_WAIT_H_

#include <sys/types.h>

#define WNOHANG 1

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)

pid_t waitpid(pid_t pid, int *status, int options);

#endif
