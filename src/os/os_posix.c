/**
 * @file os_posix.c
 * @brief libbeluga-os-posix: include/os.h for Linux, macOS and the BSDs.
 * @details Most of this is code that used to live in the editor itself —
 * raw mode from terminal.c, forkpty from pty_session.c, fork/exec/poll
 * from subprocess.c, async_process.c and completion.c, the clipboard from
 * editor_op.c — gathered behind os.h. The file operations are new: dired
 * used to run rm, mv, cp and mkdir; it now calls os_delete_tree() & co.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // forkpty, memmem, ... ; this library is allowed to be
                    // POSIX
#include <stddef.h>
#endif

#include "../../include/galloc.h"
#include "../../include/os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>     // proc_pidpath
#include <sys/sysctl.h>  // KERN_PROC_UID
#include <mach-o/dyld.h> // _NSGetExecutablePath
#include <malloc/malloc.h> // malloc_zone_statistics
#include <util.h> // forkpty
#else
#include <pty.h> // forkpty (libutil on Linux)
#if defined(__GLIBC__)
#include <malloc.h> // mallinfo2
#endif
#endif

/***********************************************************************************************************************
 *
 *                                                 Process lifetime and identity
 *
 **********************************************************************************************************************/

void os_init(void) {
  // Writing to a pipe or pty whose reader is gone (a language server or
  // shell that exited) must fail with EPIPE, not kill the editor.
  signal(SIGPIPE, SIG_IGN);
}

const char *os_name(void) {
#if defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#else
  return "unix";
#endif
}

long os_getpid(void) { return (long)getpid(); }

static void setCloexec(int fd);

static volatile sig_atomic_t s_interrupted;
static volatile sig_atomic_t s_hung_up;

static void onStopSignal(const int sig) {
  if (sig == SIGINT)
    s_interrupted = 1;
  else
    s_hung_up = 1;
}

void os_catch_stop_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onStopSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
}

int os_take_stop_signal(void) {
  if (s_hung_up) {
    s_hung_up = 0;
    s_interrupted = 0;
    return OS_STOP_HANGUP;
  }
  if (s_interrupted) {
    s_interrupted = 0;
    return OS_STOP_INTERRUPT;
  }
  return OS_STOP_NONE;
}

char *os_exe_path(void) {
  char path[OS_PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof(path);
  char *real;
  if (_NSGetExecutablePath(path, &size) != 0)
    return NULL;
  real = os_realpath(path);
  return real ? real : gstrdup(path);
#else
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0)
    return NULL;
  path[n] = '\0';
  return gstrdup(path);
#endif
}

static int compareProcesses(const void *a, const void *b) {
  const long x = ((const OsProcessInfo *)a)->pid;
  const long y = ((const OsProcessInfo *)b)->pid;
  return x < y ? -1 : x > y;
}

#ifdef __APPLE__
OsProcessInfo *os_process_list(int *count) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_UID, (int)geteuid()};
  struct kinfo_proc *procs = NULL;
  OsProcessInfo *out;
  char path[PROC_PIDPATHINFO_MAXSIZE];
  size_t size = 0;
  int n;
  int i;
  *count = 0;
  // The list can grow between asking its size and reading it: room to spare.
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return NULL;
  size += size / 4 + sizeof(struct kinfo_proc) * 16;
  procs = galloc(size);
  if (!procs)
    return NULL;
  if (sysctl(mib, 4, procs, &size, NULL, 0) != 0) {
    gfree(procs);
    return NULL;
  }
  n = (int)(size / sizeof(struct kinfo_proc));
  out = gcalloc(n > 0 ? (size_t)n : 1, sizeof(OsProcessInfo));
  if (!out) {
    gfree(procs);
    return NULL;
  }
  for (i = 0; i < n; i++) {
    if (procs[i].kp_proc.p_pid == getpid() || procs[i].kp_proc.p_pid <= 0)
      continue;
    out[*count].pid = procs[i].kp_proc.p_pid;
    if (proc_pidpath(procs[i].kp_proc.p_pid, path, sizeof(path)) > 0)
      snprintf(out[*count].command, sizeof(out[*count].command), "%s", path);
    else
      snprintf(out[*count].command, sizeof(out[*count].command), "%s", procs[i].kp_proc.p_comm);
    (*count)++;
  }
  gfree(procs);
  qsort(out, (size_t)*count, sizeof(OsProcessInfo), compareProcesses);
  return out;
}
#else
OsProcessInfo *os_process_list(int *count) {
  DIR *dir = opendir("/proc");
  struct dirent *entry;
  struct stat st;
  OsProcessInfo *out = NULL;
  OsProcessInfo *grown;
  char path[64];
  char *end;
  FILE *f;
  size_t n;
  size_t i;
  long pid;
  int cap = 0;
  *count = 0;
  if (!dir)
    return NULL;
  while ((entry = readdir(dir)) != NULL) {
    pid = strtol(entry->d_name, &end, 10);
    if (*end || pid <= 0 || pid == (long)getpid())
      continue;
    snprintf(path, sizeof(path), "/proc/%ld", pid);
    if (stat(path, &st) != 0 || st.st_uid != geteuid())
      continue;
    if (*count == cap) {
      grown = grealloc(out, sizeof(OsProcessInfo) * (size_t)(cap ? cap * 2 : 128));
      if (!grown)
        break;
      out = grown;
      cap = cap ? cap * 2 : 128;
    }
    out[*count].pid = pid;
    out[*count].command[0] = '\0';
    // The arguments, NUL-separated; empty for a kernel thread.
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
    f = fopen(path, "r");
    n = f ? fread(out[*count].command, 1, sizeof(out[*count].command) - 1, f) : 0;
    if (f)
      fclose(f);
    while (n > 0 && out[*count].command[n - 1] == '\0')
      n--;
    for (i = 0; i < n; i++)
      if (out[*count].command[i] == '\0')
        out[*count].command[i] = ' ';
    out[*count].command[n] = '\0';
    if (n == 0)
      continue;
    (*count)++;
  }
  closedir(dir);
  if (out)
    qsort(out, (size_t)*count, sizeof(OsProcessInfo), compareProcesses);
  return out;
}
#endif

int os_spawn_detached(const char *const *argv, const char *log_file, char *err,
                      const size_t err_size) {
  int report[2]; // the grandchild's errno if exec fails; EOF once it ran
  int child_errno = 0;
  int status;
  int fd;
  pid_t pid;
  ssize_t n;

  if (!argv || !argv[0]) {
    snprintf(err, err_size, "nothing to start");
    return -1;
  }
  if (pipe(report) == -1) {
    snprintf(err, err_size, "pipe: %s", strerror(errno));
    return -1;
  }
  setCloexec(report[0]);
  setCloexec(report[1]);
  pid = fork();
  if (pid == -1) {
    snprintf(err, err_size, "fork: %s", strerror(errno));
    close(report[0]);
    close(report[1]);
    return -1;
  }
  if (pid == 0) {
    // A session of its own, then a second fork so it isn't a session
    // leader either: no terminal can ever become its controlling one.
    close(report[0]);
    setsid();
    if (fork() != 0)
      _exit(0);
    fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
      dup2(fd, 0);
      if (fd > 2)
        close(fd);
    }
    fd = log_file ? open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
    if (fd < 0)
      fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
      if (fd > 2)
        close(fd);
    }
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    execv(argv[0], (char *const *)argv);
    child_errno = errno;
    if (write(report[1], &child_errno, sizeof(child_errno)) < 0)
      _exit(127);
    _exit(127);
  }
  close(report[1]);
  waitpid(pid, &status, 0); // the first child, which exits at once
  do
    n = read(report[0], &child_errno, sizeof(child_errno));
  while (n < 0 && errno == EINTR);
  close(report[0]);
  if (n == (ssize_t)sizeof(child_errno)) {
    snprintf(err, err_size, "%s: %s", argv[0], strerror(child_errno));
    return -1;
  }
  return 0;
}

/***********************************************************************************************************************
 *
 *                                                 Time
 *
 **********************************************************************************************************************/

long long os_monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void os_sleep_ms(const int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    ;
}

/***********************************************************************************************************************
 *
 *                                                 Streams and waiting
 *
 **********************************************************************************************************************/

struct OsStream {
  int fd;     // -1 once closed
  int is_tty; // the keyboard: a 0-byte read means "nothing yet", not EOF
  int eof;    // end reached: every later read returns -1 at once
};

static void streamInit(OsStream *s, const int fd, const int is_tty) {
  s->fd = fd;
  s->is_tty = is_tty;
  s->eof = fd < 0;
  if (fd >= 0 && !is_tty)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void streamClose(OsStream *s) {
  if (s->fd >= 0)
    close(s->fd);
  s->fd = -1;
  s->eof = 1;
}

long os_stream_read(OsStream *s, void *buf, const size_t n,
                    const int timeout_ms) {
  struct pollfd pfd;
  ssize_t got;
  int pr;

  if (!s || s->eof)
    return -1;
  if (n == 0)
    return 0;

  pfd.fd = s->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  do
    pr = poll(&pfd, 1, timeout_ms);
  while (pr < 0 && errno == EINTR);
  if (pr == 0)
    return 0;

  for (;;) {
    got = read(s->fd, buf, n);
    if (got > 0)
      return (long)got;
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return 0;
    // 0 from a pipe: the writer is gone. -1 EIO from a pty master:
    // the shell has exited. A tty in raw mode (VMIN=0) also returns 0
    // when nothing came in, which is not an end.
    if (got == 0 && s->is_tty)
      return 0;
    s->eof = 1;
    return -1;
  }
}

int os_wait(OsStream *const *streams, const int n, const int timeout_ms,
            int *ready) {
  struct pollfd pfds[64];
  int map[64];
  int count = 0;
  int total = 0;
  int pr;
  int i;

  for (i = 0; i < n; i++) {
    ready[i] = 0;
    if (!streams[i])
      continue;
    if (streams[i]->eof) {
      ready[i] = 1; // reading it returns -1 at once
      total++;
      continue;
    }
    if (count < 64) {
      pfds[count].fd = streams[i]->fd;
      pfds[count].events = POLLIN;
      pfds[count].revents = 0;
      map[count++] = i;
    }
  }
  if (total > 0)
    return total;

  do
    pr = poll(pfds, (nfds_t)count, timeout_ms);
  while (pr < 0 && errno == EINTR);
  if (pr <= 0)
    return 0;

  for (i = 0; i < count; i++)
    if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
      ready[map[i]] = 1;
      total++;
    }
  return total;
}

/***********************************************************************************************************************
 *
 *                                                 The terminal the editor runs
 * in
 *
 **********************************************************************************************************************/

static struct termios s_orig_termios;
static int s_raw;
static OsStream s_term_input = {STDIN_FILENO, 1, 0};

int os_term_enter_raw(void) {
  struct termios raw;
  if (tcgetattr(STDIN_FILENO, &s_orig_termios) == -1)
    return -1;

  raw = s_orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  // read() returns after 100ms with whatever came in (possibly nothing):
  // os_stream_read()'s timeouts are layered on top with poll().
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    return -1;
  s_raw = 1;
  return 0;
}

void os_term_leave_raw(void) {
  if (s_raw)
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
  s_raw = 0;
}

void os_term_write(const void *buf, const size_t n) {
  const char *p = buf;
  size_t left = n;
  ssize_t w;
  while (left > 0) {
    w = write(STDOUT_FILENO, p, left);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return;
    p += w;
    left -= (size_t)w;
  }
}

/**
 * @brief Where the cursor is, asked of the terminal itself (ESC [ 6 n) —
 * the fallback for terminals that don't answer TIOCGWINSZ.
 */
static int cursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  os_term_write("\x1b[6n", 4);
  while (i < sizeof(buf) - 1) {
    if (os_stream_read(&s_term_input, &buf[i], 1, 1000) != 1)
      break;
    if (buf[i] == 'R')
      break;
    ++i;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  return sscanf(&buf[2], "%d;%d", rows, cols) == 2 ? 0 : -1;
}

int os_term_size(int *rows, int *cols) {
  struct winsize ws;
  static int asked = 0; // the cursor-position fallback, done once
  static int asked_result = -1;
  static int asked_rows = 0;
  static int asked_cols = 0;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // No size from the terminal driver (some serial consoles, a pty whose
    // size was never set). Push the cursor to the bottom-right corner and
    // ask where it is — once: the run loop polls the size every tick, and
    // asking each time read the keyboard for up to a second per tick,
    // eating keystrokes as if they were the terminal's reply.
    if (!asked) {
      asked = 1;
      os_term_write("\x1b[999C\x1b[999B", 12);
      asked_result = cursorPosition(&asked_rows, &asked_cols);
    }
    if (asked_result == 0) {
      *rows = asked_rows;
      *cols = asked_cols;
    }
    return asked_result;
  }
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

OsStream *os_term_input(void) { return &s_term_input; }

/***********************************************************************************************************************
 *
 *                                                 Programs
 *
 **********************************************************************************************************************/

struct OsProc {
  pid_t pid;
  OsStream out;
  int in_fd; // -1 without OsSpawn.stdin_pipe
  int exited;
  int exit_code;
};

static void setCloexec(const int fd) {
  fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
}

/**
 * @brief In the child, between fork() and exec(): never returns.
 */
static void execChild(const OsSpawn *o, const int out_w, const int in_r) {
  int devnull = open("/dev/null", O_RDWR);
  int err_fd;

  // A session of its own (no_prompt): no controlling terminal, so
  // ssh/git/gpg can't open /dev/tty to prompt over the editor. Either
  // way its own process group, which os_proc_kill() ends as a whole.
  if (o->no_prompt)
    setsid();
  else
    setpgid(0, 0);

  if (o->cwd && chdir(o->cwd) != 0)
    _exit(127);

  dup2(in_r >= 0 ? in_r : devnull, STDIN_FILENO);
  dup2(out_w, STDOUT_FILENO);
  switch (o->stderr_mode) {
  case OS_STDERR_MERGE:
    dup2(out_w, STDERR_FILENO);
    break;
  case OS_STDERR_FILE:
    err_fd = o->stderr_file
                 ? open(o->stderr_file, O_WRONLY | O_CREAT | O_APPEND, 0644)
                 : -1;
    dup2(err_fd >= 0 ? err_fd : devnull, STDERR_FILENO);
    break;
  default:
    dup2(devnull, STDERR_FILENO);
    break;
  }
  execvp(o->argv[0], (char *const *)o->argv);
  _exit(127); // not found, or not executable
}

OsProc *os_proc_spawn(const OsSpawn *o) {
  int out[2];
  int in[2] = {-1, -1};
  OsProc *p;
  pid_t pid;

  if (!o || !o->argv || !o->argv[0])
    return NULL;
  p = gcalloc(1, sizeof(*p));
  if (!p)
    return NULL;
  if (pipe(out) != 0 || (o->stdin_pipe && pipe(in) != 0)) {
    gfree(p);
    return NULL;
  }

  pid = fork();
  if (pid < 0) {
    close(out[0]);
    close(out[1]);
    if (in[0] >= 0) {
      close(in[0]);
      close(in[1]);
    }
    gfree(p);
    return NULL;
  }
  if (pid == 0) {
    close(out[0]);
    if (in[1] >= 0)
      close(in[1]);
    execChild(o, out[1], in[0]);
  }

  // Mirrors the child's own setpgid(): avoids the race where a kill of
  // the group could come before the child has joined it.
  if (!o->no_prompt)
    setpgid(pid, pid);
  close(out[1]);
  if (in[0] >= 0)
    close(in[0]);
  // Our ends must not leak into programs started later: an inherited
  // copy of a pipe's write end keeps it from ever reaching EOF.
  setCloexec(out[0]);
  if (in[1] >= 0)
    setCloexec(in[1]);

  p->pid = pid;
  streamInit(&p->out, out[0], 0);
  p->in_fd = in[1];
  p->exit_code = -1;
  return p;
}

OsStream *os_proc_output(OsProc *p) { return p ? &p->out : NULL; }

long os_proc_write(OsProc *p, const void *buf, const size_t n) {
  const char *c = buf;
  size_t left = n;
  ssize_t w;
  if (!p || p->in_fd < 0)
    return -1;
  while (left > 0) {
    w = write(p->in_fd, c, left);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return -1;
    c += w;
    left -= (size_t)w;
  }
  return (long)n;
}

void os_proc_kill(OsProc *p) {
  if (!p || p->exited)
    return;
  kill(-p->pid, SIGKILL); // its process group: the program and its children
  kill(p->pid, SIGKILL);  // in case it left the group (setsid)
}

/**
 * @brief Reaps `p` if it has exited; `options` as for waitpid().
 */
static int reap(OsProc *p, const int options) {
  int status;
  pid_t r;
  if (p->exited)
    return 1;
  do
    r = waitpid(p->pid, &status, options);
  while (r < 0 && errno == EINTR);
  if (r == 0)
    return 0;
  p->exited = 1;
  p->exit_code = (r == p->pid && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
  return 1;
}

int os_proc_wait(OsProc *p, const int timeout_ms, int *exit_code) {
  long long deadline;
  int done;

  if (!p)
    return 1;
  if (timeout_ms < 0)
    done = reap(p, 0);
  else {
    deadline = os_monotonic_ms() + timeout_ms;
    while (!(done = reap(p, WNOHANG)) && os_monotonic_ms() < deadline)
      os_sleep_ms(5);
  }
  if (done && exit_code)
    *exit_code = p->exit_code;
  return done;
}

void os_proc_free(OsProc *p) {
  if (!p)
    return;
  streamClose(&p->out);
  if (p->in_fd >= 0)
    close(p->in_fd);
  if (!reap(p, WNOHANG)) {
    os_proc_kill(p);
    reap(p, 0);
  }
  gfree(p);
}

/**
 * @brief Is `path` a regular file this user may execute?
 */
static int isExecutableFile(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

char *os_find_program(const char *name) {
  const char *path = getenv("PATH");
  const char *dir;
  const char *end;
  char full[OS_PATH_MAX];
  size_t dir_len;

  if (!name || !*name)
    return NULL;
  if (strchr(name, '/'))
    return isExecutableFile(name) ? gstrdup(name) : NULL;
  if (!path)
    path = "/usr/bin:/bin";
  for (dir = path; *dir; dir = *end ? end + 1 : end) {
    end = strchr(dir, ':');
    if (!end)
      end = dir + strlen(dir);
    dir_len = (size_t)(end - dir);
    // An empty entry is the current directory, which execvp() would
    // search: the same here, so the answer matches what spawning does.
    if (dir_len == 0)
      snprintf(full, sizeof(full), "./%s", name);
    else if (snprintf(full, sizeof(full), "%.*s/%s", (int)dir_len, dir, name) >=
             (int)sizeof(full))
      continue;
    if (isExecutableFile(full))
      return gstrdup(full);
  }
  return NULL;
}

char **os_shell_argv(const char *command) {
  char **argv = gcalloc(4, sizeof(char *));
  if (!argv)
    return NULL;
  argv[0] = gstrdup("/bin/sh");
  argv[1] = gstrdup("-c");
  argv[2] = gstrdup(command);
  if (!argv[0] || !argv[1] || !argv[2]) {
    os_free_argv(argv);
    return NULL;
  }
  return argv;
}

void os_free_argv(char **argv) {
  char **a;
  if (!argv)
    return;
  for (a = argv; *a; a++)
    gfree(*a);
  gfree(argv);
}

char *os_shell_quote(const char *s) {
  // 'it'\''s': single quotes keep everything literal; an embedded quote
  // closes them, adds an escaped quote, and reopens.
  size_t len = 2;
  const char *c;
  char *out;
  char *o;
  for (c = s; *c; c++)
    len += *c == '\'' ? 4 : 1;
  out = galloc(len + 1);
  if (!out)
    return NULL;
  o = out;
  *o++ = '\'';
  for (c = s; *c; c++) {
    if (*c == '\'') {
      memcpy(o, "'\\''", 4);
      o += 4;
    } else
      *o++ = *c;
  }
  *o++ = '\'';
  *o = '\0';
  return out;
}

/***********************************************************************************************************************
 *
 *                                                 Terminal panes
 *
 **********************************************************************************************************************/

struct OsPty {
  pid_t pid;
  OsStream out;
  int exited;
};

/**
 * @brief forkpty() for os_pty_open() and os_pty_spawn(): the child changes
 * to `cwd`, applies `env` and TERM, then runs `argv` (execvp) or, with
 * `shell` set instead, the shell interactively.
 */
static OsPty *ptyStart(const char *shell, const char *const *argv, const char *cwd,
                       const char *const *env, const int rows, const int cols) {
  struct winsize ws;
  OsPty *p;
  pid_t pid;
  int master;
  const char *sh;
  const char *eq;
  char name[256];
  int i;

  p = gcalloc(1, sizeof(*p));
  if (!p)
    return NULL;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = (unsigned short)rows;
  ws.ws_col = (unsigned short)cols;

  pid = forkpty(&master, NULL, NULL, &ws);
  if (pid < 0) {
    gfree(p);
    return NULL;
  }
  if (pid == 0) {
    if (cwd && *cwd && chdir(cwd) != 0) {
      // Not there any more: the editor's directory, as before.
    }
    for (i = 0; env && env[i]; i++) {
      eq = strchr(env[i], '=');
      if (!eq) {
        unsetenv(env[i]);
      } else if ((size_t)(eq - env[i]) < sizeof(name)) {
        memcpy(name, env[i], (size_t)(eq - env[i]));
        name[eq - env[i]] = '\0';
        setenv(name, eq + 1, 1);
      }
    }
    setenv("TERM", "xterm-256color", 1);
    if (argv) {
      execvp(argv[0], (char *const *)argv);
      fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
      _exit(127);
    }
    // The child: become the shell.
    sh = (shell && *shell) ? shell : getenv("SHELL");
    if (!sh || !*sh)
      sh = access("/bin/zsh", X_OK) == 0 ? "/bin/zsh" : "/bin/sh";
    execl(sh, sh, "-i", (char *)NULL);
    _exit(127);
  }

  setCloexec(master);
  p->pid = pid;
  streamInit(&p->out, master, 0);
  return p;
}

OsPty *os_pty_open(const char *shell, const char *cwd, const int rows, const int cols) {
  return ptyStart(shell ? shell : "", NULL, cwd, NULL, rows, cols);
}

OsPty *os_pty_spawn(const char *const *argv, const char *cwd, const char *const *env,
                    const int rows, const int cols) {
  if (!argv || !argv[0])
    return NULL;
  return ptyStart(NULL, argv, cwd, env, rows, cols);
}

long os_pty_pid(OsPty *p) { return p ? (long)p->pid : -1; }

OsStream *os_pty_output(OsPty *p) { return p ? &p->out : NULL; }

long os_pty_write(OsPty *p, const void *buf, const size_t n) {
  const char *c = buf;
  size_t left = n;
  ssize_t w;
  if (!p || p->out.fd < 0)
    return -1;
  while (left > 0) {
    w = write(p->out.fd, c, left);
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      os_sleep_ms(1); // the master is non-blocking; the shell is busy
      continue;
    }
    if (w <= 0)
      return -1;
    c += w;
    left -= (size_t)w;
  }
  return (long)n;
}

void os_pty_resize(OsPty *p, const int rows, const int cols) {
  struct winsize ws;
  if (!p || p->out.fd < 0)
    return;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = (unsigned short)rows;
  ws.ws_col = (unsigned short)cols;
  ioctl(p->out.fd, TIOCSWINSZ, &ws);
  if (!p->exited)
    kill(p->pid, SIGWINCH);
}

int os_pty_alive(OsPty *p) {
  int status;
  pid_t r;
  if (!p || p->exited)
    return 0;
  r = waitpid(p->pid, &status, WNOHANG);
  if (r == 0)
    return 1;
  p->exited = 1; // exited (r == pid) or not ours to wait for: either way, done
  return 0;
}

void os_pty_close(OsPty *p) {
  if (!p)
    return;
  if (!p->exited) {
    kill(p->pid, SIGHUP);
    waitpid(p->pid, NULL, 0);
  }
  streamClose(&p->out);
  gfree(p);
}

/***********************************************************************************************************************
 *
 *                                                 Local connections (daemon mode)
 *
 **********************************************************************************************************************/

struct OsServer {
  OsStream listen; // ready when a client is waiting
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct OsConn {
  OsStream in;
};

static void setError(char *err, const size_t err_size, const char *what,
                     const char *path) {
  if (err && err_size > 0)
    snprintf(err, err_size, "%s %s: %s", what, path, strerror(errno));
}

/**
 * @brief The private directory the sockets live in, created if needed:
 * $XDG_RUNTIME_DIR/beluga, else $TMPDIR (or /tmp) /beluga-<uid> — the same
 * scheme emacs uses. Refused unless it is a real directory that belongs to
 * us and nobody else can enter, since anyone who can reach the socket can
 * drive the editor.
 */
static int socketDir(char *dir, const size_t size, char *err,
                     const size_t err_size) {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  const char *tmp = getenv("TMPDIR");
  struct stat st;
  size_t len;

  if (xdg && xdg[0] == '/')
    snprintf(dir, size, "%s/beluga", xdg);
  else
    snprintf(dir, size, "%s/beluga-%ld", (tmp && tmp[0] == '/') ? tmp : "/tmp",
             (long)getuid());
  // $TMPDIR on macOS ends with a '/'.
  len = strlen(dir);
  while (len > 1 && dir[len - 1] == '/' && dir[len - 2] == '/')
    dir[--len] = '\0';

  if (mkdir(dir, 0700) == -1 && errno != EEXIST) {
    setError(err, err_size, "can't create", dir);
    return -1;
  }
  if (lstat(dir, &st) == -1) {
    setError(err, err_size, "can't use", dir);
    return -1;
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 077) != 0) {
    if (err && err_size > 0)
      snprintf(err, err_size,
               "%s must be a directory of yours that only you can open "
               "(chmod 700)",
               dir);
    return -1;
  }
  return 0;
}

static int socketAddress(const char *name, struct sockaddr_un *addr,
                         char *err, const size_t err_size) {
  char dir[sizeof(addr->sun_path)];
  int n;

  if (!name || !name[0] || strchr(name, '/')) {
    if (err && err_size > 0)
      snprintf(err, err_size, "invalid daemon name \"%s\"", name ? name : "");
    return -1;
  }
  if (socketDir(dir, sizeof(dir), err, err_size) == -1)
    return -1;
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  n = snprintf(addr->sun_path, sizeof(addr->sun_path), "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= sizeof(addr->sun_path)) {
    if (err && err_size > 0)
      snprintf(err, err_size, "socket path too long: %s/%s", dir, name);
    return -1;
  }
  return 0;
}

static int connectTo(const struct sockaddr_un *addr) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;
  if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == -1) {
    close(fd);
    return -1;
  }
  return fd;
}

OsServer *os_server_listen(const char *name, char *err, const size_t err_size) {
  struct sockaddr_un addr;
  OsServer *s;
  int fd;
  int other;

  if (socketAddress(name, &addr, err, err_size) == -1)
    return NULL;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    setError(err, err_size, "can't create a socket for", addr.sun_path);
    return NULL;
  }
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    if (errno != EADDRINUSE) {
      setError(err, err_size, "can't listen on", addr.sun_path);
      close(fd);
      return NULL;
    }
    // Someone answering there is a running daemon; nobody answering is
    // the socket of one that crashed.
    other = connectTo(&addr);
    if (other != -1) {
      close(other);
      close(fd);
      if (err && err_size > 0)
        snprintf(err, err_size, "a beluga daemon called \"%s\" is already running",
                 name);
      return NULL;
    }
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
      setError(err, err_size, "can't listen on", addr.sun_path);
      close(fd);
      return NULL;
    }
  }
  if (listen(fd, 16) == -1) {
    setError(err, err_size, "can't listen on", addr.sun_path);
    close(fd);
    unlink(addr.sun_path);
    return NULL;
  }
  setCloexec(fd);

  s = gcalloc(1, sizeof(*s));
  streamInit(&s->listen, fd, 0);
  memcpy(s->path, addr.sun_path, sizeof(s->path));
  return s;
}

OsStream *os_server_stream(OsServer *s) { return s ? &s->listen : NULL; }

static OsConn *connFromFd(const int fd) {
  OsConn *c = gcalloc(1, sizeof(*c));
  setCloexec(fd);
  streamInit(&c->in, fd, 0);
  return c;
}

OsConn *os_server_accept(OsServer *s) {
  int fd;
  if (!s || s->listen.fd < 0)
    return NULL;
  do
    fd = accept(s->listen.fd, NULL, NULL);
  while (fd == -1 && errno == EINTR);
  return fd == -1 ? NULL : connFromFd(fd);
}

void os_server_close(OsServer *s) {
  if (!s)
    return;
  streamClose(&s->listen);
  unlink(s->path);
  gfree(s);
}

OsConn *os_conn_connect(const char *name, char *err, const size_t err_size) {
  struct sockaddr_un addr;
  int fd;

  if (socketAddress(name, &addr, err, err_size) == -1)
    return NULL;
  fd = connectTo(&addr);
  if (fd == -1) {
    if (err && err_size > 0)
      snprintf(err, err_size, "no beluga daemon called \"%s\" is running (%s)",
               name, addr.sun_path);
    return NULL;
  }
  return connFromFd(fd);
}

/**
 * @brief connect() on a non-blocking socket, waiting at most `timeout_ms`.
 * @return 0, or -1 with errno set.
 */
static int connectWithin(const int fd, const struct sockaddr *addr, const socklen_t len,
                         const int timeout_ms) {
  struct pollfd pfd;
  int error = 0;
  socklen_t error_len = sizeof(error);
  int pr;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  if (connect(fd, addr, len) == 0)
    return 0;
  if (errno != EINPROGRESS)
    return -1;
  pfd.fd = fd;
  pfd.events = POLLOUT;
  pfd.revents = 0;
  do
    pr = poll(&pfd, 1, timeout_ms);
  while (pr < 0 && errno == EINTR);
  if (pr == 0) {
    errno = ETIMEDOUT;
    return -1;
  }
  if (pr < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == -1)
    return -1;
  if (error != 0) {
    errno = error;
    return -1;
  }
  return 0;
}

OsConn *os_tcp_connect(const char *host, const int port, const int timeout_ms, char *err,
                       const size_t err_size) {
  struct addrinfo hints;
  struct addrinfo *found = NULL;
  struct addrinfo *a;
  char service[16];
  int one = 1;
  int fd = -1;
  int rc;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(service, sizeof(service), "%d", port);
  rc = getaddrinfo(host, service, &hints, &found);
  if (rc != 0) {
    snprintf(err, err_size, "%s: %s", host, gai_strerror(rc));
    return NULL;
  }
  errno = ECONNREFUSED;
  for (a = found; a && fd == -1; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd == -1)
      continue;
    if (connectWithin(fd, a->ai_addr, a->ai_addrlen, timeout_ms) == -1) {
      rc = errno;
      close(fd);
      fd = -1;
      errno = rc;
    }
  }
  freeaddrinfo(found);
  if (fd == -1) {
    snprintf(err, err_size, "%s:%d: %s", host, port, strerror(errno));
    return NULL;
  }
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return connFromFd(fd);
}

int os_tcp_free_port(void) {
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  int port = -1;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0; // the system picks one
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
      getsockname(fd, (struct sockaddr *)&addr, &len) == 0)
    port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

OsStream *os_conn_input(OsConn *c) { return c ? &c->in : NULL; }

long os_conn_write(OsConn *c, const void *buf, const size_t n) {
  const char *p = buf;
  size_t left = n;
  struct pollfd pfd;
  ssize_t w;
  int pr;

  if (!c || c->in.fd < 0)
    return -1;
  while (left > 0) {
    w = write(c->in.fd, p, left);
    if (w > 0) {
      p += w;
      left -= (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // The socket is non-blocking (it shares the fd with the reading
      // side). A client that stops reading for 5 s is given up on rather
      // than freezing every other window.
      pfd.fd = c->in.fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      do
        pr = poll(&pfd, 1, 5000);
      while (pr < 0 && errno == EINTR);
      if (pr > 0 && !(pfd.revents & (POLLERR | POLLHUP)))
        continue;
    }
    return -1;
  }
  return (long)n;
}

void os_conn_close(OsConn *c) {
  if (!c)
    return;
  streamClose(&c->in);
  gfree(c);
}

/***********************************************************************************************************************
 *
 *                                                 Files and directories
 *
 **********************************************************************************************************************/

OsDirEntry *os_list_dir(const char *path, int *count) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  struct stat st;
  OsDirEntry *list;
  OsDirEntry *grown;
  char full[OS_PATH_MAX];
  int cap = 64;
  int n = 0;

  *count = 0;
  if (!dir)
    return NULL;
  list = galloc((size_t)cap * sizeof(*list));
  if (!list) {
    closedir(dir);
    return NULL;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (n == cap) {
      cap *= 2;
      grown = grealloc(list, (size_t)cap * sizeof(*list));
      if (!grown)
        break;
      list = grown;
    }
    snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
    list[n].name = gstrdup(entry->d_name);
    if (!list[n].name)
      break;
    list[n].is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    n++;
  }
  closedir(dir);
  *count = n;
  return list;
}

void os_free_dir(OsDirEntry *entries, const int count) {
  int i;
  if (!entries)
    return;
  for (i = 0; i < count; i++)
    gfree(entries[i].name);
  gfree(entries);
}

/**
 * @brief An `ls -l` style mode string ("drwxr-xr-x") from st_mode.
 */
static void modeString(const mode_t mode, char out[11]) {
  if (S_ISDIR(mode))
    out[0] = 'd';
  else if (S_ISLNK(mode))
    out[0] = 'l';
  else if (S_ISCHR(mode))
    out[0] = 'c';
  else if (S_ISBLK(mode))
    out[0] = 'b';
  else if (S_ISFIFO(mode))
    out[0] = 'p';
  else if (S_ISSOCK(mode))
    out[0] = 's';
  else
    out[0] = '-';
  out[1] = (mode & S_IRUSR) ? 'r' : '-';
  out[2] = (mode & S_IWUSR) ? 'w' : '-';
  out[3] = (mode & S_ISUID) ? ((mode & S_IXUSR) ? 's' : 'S')
                            : ((mode & S_IXUSR) ? 'x' : '-');
  out[4] = (mode & S_IRGRP) ? 'r' : '-';
  out[5] = (mode & S_IWGRP) ? 'w' : '-';
  out[6] = (mode & S_ISGID) ? ((mode & S_IXGRP) ? 's' : 'S')
                            : ((mode & S_IXGRP) ? 'x' : '-');
  out[7] = (mode & S_IROTH) ? 'r' : '-';
  out[8] = (mode & S_IWOTH) ? 'w' : '-';
  out[9] = (mode & S_ISVTX) ? ((mode & S_IXOTH) ? 't' : 'T')
                            : ((mode & S_IXOTH) ? 'x' : '-');
  out[10] = '\0';
}

int os_file_info(const char *path, OsFileInfo *info) {
  struct stat lst;
  struct stat st;
  ssize_t len;

  memset(info, 0, sizeof(*info));
  if (lstat(path, &lst) != 0)
    return -1;
  info->is_link = S_ISLNK(lst.st_mode);
  info->is_dir = info->is_link ? (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                               : S_ISDIR(lst.st_mode);
  info->size = (long long)lst.st_size;
  info->mtime = (long long)lst.st_mtime;
  modeString(lst.st_mode, info->mode);
  if (info->is_link) {
    len = readlink(path, info->link_target, sizeof(info->link_target) - 1);
    info->link_target[len > 0 ? len : 0] = '\0';
  }
  return 0;
}

int os_file_flush(FILE *f) {
  if (fflush(f) != 0)
    return -1;
  return fsync(fileno(f)) == 0 ? 0 : -1;
}

int os_replace_file(const char *tmp, const char *dest, char *err,
                    size_t err_size) {
  struct stat st;
  // Same rule as writing it in place: a file without write permission
  // isn't replaced, even though the directory would allow the rename.
  if (access(dest, F_OK) == 0 && access(dest, W_OK) != 0) {
    snprintf(err, err_size, "%s is read-only", dest);
    return -1;
  }
  // Keep the original's mode (an executable script stays executable). Its
  // owner can't be kept without privileges; the group is tried.
  if (stat(dest, &st) == 0) {
    chmod(tmp, st.st_mode & 07777);
    if (chown(tmp, (uid_t)-1, st.st_gid) != 0) {
      // not the group's member: the file gets the user's default group
    }
  }
  if (rename(tmp, dest) != 0) {
    snprintf(err, err_size, "%s", strerror(errno));
    return -1;
  }
  return 0;
}

// realpath() allocates with the C library's malloc; hand back a galloc'd
// copy so the caller's gfree() and the memory inspector see it.
char *os_realpath(const char *path) {
  char *raw = realpath(path, NULL);
  char *copy;
  if (!raw)
    return NULL;
  copy = gstrdup(raw);
  gfree(raw); // galloc: raw — realpath() malloc'd it
  return copy;
}

char *os_getcwd(void) {
  char buf[OS_PATH_MAX];
  return getcwd(buf, sizeof(buf)) ? gstrdup(buf) : NULL;
}

char *os_home_dir(void) {
  const char *home = getenv("HOME");
  return gstrdup((home && *home) ? home : "/");
}

int os_path_is_absolute(const char *path) { return path && path[0] == '/'; }

int os_path_drive_length(const char *path) {
  (void)path;
  return 0;
}

void os_path_to_slashes(char *path) { (void)path; }

int os_path_equal(const char *a, const char *b) { return strcmp(a, b) == 0; }

// ─── File operations ────────────────────────────────────────────────────────

static int fail(char *err, const size_t err_size, const char *path) {
  if (err && err_size)
    snprintf(err, err_size, "%s: %s", path, strerror(errno));
  return -1;
}

int os_delete_tree(const char *path, char *err, const size_t err_size) {
  struct stat st;
  DIR *dir;
  struct dirent *entry;
  char child[OS_PATH_MAX];
  int rc = 0;

  if (err && err_size)
    err[0] = '\0';
  if (lstat(path, &st) != 0)
    return fail(err, err_size, path);
  if (!S_ISDIR(st.st_mode)) // files and links (never followed)
    return unlink(path) == 0 ? 0 : fail(err, err_size, path);

  dir = opendir(path);
  if (!dir)
    return fail(err, err_size, path);
  while (rc == 0 && (entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    rc = os_delete_tree(child, err, err_size);
  }
  closedir(dir);
  if (rc != 0)
    return rc;
  return rmdir(path) == 0 ? 0 : fail(err, err_size, path);
}

int os_mkdirs(const char *path, char *err, const size_t err_size) {
  char buf[OS_PATH_MAX];
  struct stat st;
  char *p;
  char saved;

  if (err && err_size)
    err[0] = '\0';
  snprintf(buf, sizeof(buf), "%s", path);
  for (p = buf + 1;; p++) {
    if (*p == '/' || *p == '\0') {
      saved = *p;
      *p = '\0';
      if (mkdir(buf, 0777) != 0 &&
          !(errno == EEXIST && stat(buf, &st) == 0 && S_ISDIR(st.st_mode)))
        return fail(err, err_size, buf);
      *p = saved;
      if (saved == '\0')
        break;
    }
  }
  return 0;
}

/**
 * @brief Where `src` goes for mv/cp into `dest`: inside it when it's an
 * existing directory, else `dest` itself.
 */
static void targetPath(const char *src, const char *dest, const int dest_is_dir,
                       char *out, const size_t out_size) {
  const char *base;
  size_t len = strlen(src);
  while (len > 1 && src[len - 1] == '/')
    len--;
  base = src + len;
  while (base > src && base[-1] != '/')
    base--;
  if (dest_is_dir)
    snprintf(out, out_size, "%s/%.*s", dest, (int)(src + len - base), base);
  else
    snprintf(out, out_size, "%s", dest);
}

static int copyFile(const char *src, const char *dst, const mode_t mode,
                    char *err, const size_t err_size) {
  char buf[65536];
  ssize_t n;
  ssize_t w;
  ssize_t off;
  int in = open(src, O_RDONLY);
  int out;
  int rc = 0;

  if (in < 0)
    return fail(err, err_size, src);
  out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777);
  if (out < 0) {
    close(in);
    return fail(err, err_size, dst);
  }
  while (rc == 0 && (n = read(in, buf, sizeof(buf))) != 0) {
    if (n < 0) {
      if (errno == EINTR)
        continue;
      rc = fail(err, err_size, src);
      break;
    }
    for (off = 0; off < n; off += w) {
      w = write(out, buf + off, (size_t)(n - off));
      if (w < 0 && errno == EINTR) {
        w = 0;
        continue;
      }
      if (w <= 0) {
        rc = fail(err, err_size, dst);
        break;
      }
    }
  }
  close(in);
  if (close(out) != 0 && rc == 0)
    rc = fail(err, err_size, dst);
  return rc;
}

static int copyEntry(const char *src, const char *dst, char *err,
                     const size_t err_size) {
  struct stat st;
  char target[OS_PATH_MAX];
  char child_src[OS_PATH_MAX];
  char child_dst[OS_PATH_MAX];
  struct dirent *entry;
  ssize_t len;
  DIR *dir;
  int rc = 0;

  if (lstat(src, &st) != 0)
    return fail(err, err_size, src);
  if (S_ISLNK(st.st_mode)) {
    len = readlink(src, target, sizeof(target) - 1);
    if (len < 0)
      return fail(err, err_size, src);
    target[len] = '\0';
    return symlink(target, dst) == 0 ? 0 : fail(err, err_size, dst);
  }
  if (!S_ISDIR(st.st_mode))
    return copyFile(src, dst, st.st_mode, err, err_size);

  if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST)
    return fail(err, err_size, dst);
  dir = opendir(src);
  if (!dir)
    return fail(err, err_size, src);
  while (rc == 0 && (entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    snprintf(child_src, sizeof(child_src), "%s/%s", src, entry->d_name);
    snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, entry->d_name);
    rc = copyEntry(child_src, child_dst, err, err_size);
  }
  closedir(dir);
  return rc;
}

/**
 * @brief Refuses copying or moving a directory into itself, which would
 * recurse forever (cp -R a a/b).
 */
static int insideItself(const char *src, const char *target) {
  char *rs = os_realpath(src);
  char parent[OS_PATH_MAX];
  char *rp;
  char *slash;
  size_t len;
  int inside = 0;

  if (!rs)
    return 0;
  snprintf(parent, sizeof(parent), "%s", target);
  slash = strrchr(parent, '/');
  if (slash && slash != parent)
    *slash = '\0';
  rp = os_realpath(parent);
  if (rp) {
    len = strlen(rs);
    inside = strncmp(rp, rs, len) == 0 && (rp[len] == '/' || rp[len] == '\0');
    gfree(rp);
  }
  gfree(rs);
  return inside;
}

static int transfer(const int move, const char *const *sources, const int count,
                    const char *dest, char *err, const size_t err_size) {
  struct stat st;
  char target[OS_PATH_MAX];
  int dest_is_dir;
  int i;

  if (err && err_size)
    err[0] = '\0';
  dest_is_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
  if (count > 1 && !dest_is_dir) {
    snprintf(err, err_size, "%s: not a directory", dest);
    return -1;
  }
  for (i = 0; i < count; i++) {
    targetPath(sources[i], dest, dest_is_dir, target, sizeof(target));
    if (insideItself(sources[i], target)) {
      snprintf(err, err_size, "can't put %s inside itself", sources[i]);
      return -1;
    }
    if (move) {
      if (rename(sources[i], target) == 0)
        continue;
      if (errno != EXDEV)
        return fail(err, err_size, sources[i]);
      // Another file system: copy, then remove the original.
      if (copyEntry(sources[i], target, err, err_size) != 0 ||
          os_delete_tree(sources[i], err, err_size) != 0)
        return -1;
    } else if (copyEntry(sources[i], target, err, err_size) != 0)
      return -1;
  }
  return 0;
}

int os_move(const char *const *sources, const int count, const char *dest,
            char *err, const size_t err_size) {
  return transfer(1, sources, count, dest, err, err_size);
}

int os_copy_tree(const char *const *sources, const int count, const char *dest,
                 char *err, const size_t err_size) {
  return transfer(0, sources, count, dest, err, err_size);
}

/***********************************************************************************************************************
 *
 *                                                 Heap
 *
 **********************************************************************************************************************/

int os_heap_in_use(long long *blocks, long long *bytes) {
#if defined(__APPLE__)
  malloc_statistics_t st;
  malloc_zone_statistics(NULL, &st); // NULL: every zone, the whole process
  *blocks = (long long)st.blocks_in_use;
  *bytes = (long long)st.size_in_use;
  return 0;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  struct mallinfo2 mi = mallinfo2();
  *blocks = -1;
  *bytes = (long long)(mi.uordblks + mi.hblkhd); // arena + mmap'd chunks
  return 0;
#else
  *blocks = -1;
  *bytes = -1;
  return -1;
#endif
}

/***********************************************************************************************************************
 *
 *                                                 Clipboard
 *
 **********************************************************************************************************************/

#ifdef __APPLE__
#define CLIPBOARD_COPY_CMD "pbcopy"
#define CLIPBOARD_PASTE_CMD "pbpaste"
#else
#define CLIPBOARD_COPY_CMD "xclip -selection clipboard 2>/dev/null"
#define CLIPBOARD_PASTE_CMD "xclip -selection clipboard -o 2>/dev/null"
#endif

char *os_clipboard_get(void) {
  FILE *pipe = popen(CLIPBOARD_PASTE_CMD, "r");
  size_t cap = 4096;
  size_t len = 0;
  size_t i;
  size_t o;
  char *buf;
  char *grown;
  int c;

  if (!pipe)
    return NULL;
  buf = galloc(cap);
  if (!buf) {
    pclose(pipe);
    return NULL;
  }
  while ((c = fgetc(pipe)) != EOF) {
    if (len + 1 >= cap) {
      cap *= 2;
      grown = grealloc(buf, cap);
      if (!grown)
        break;
      buf = grown;
    }
    buf[len++] = (char)c;
  }
  buf[len] = '\0';
  pclose(pipe);

  for (i = o = 0; i < len; i++) // CRLF -> LF
    if (!(buf[i] == '\r' && buf[i + 1] == '\n'))
      buf[o++] = buf[i];
  buf[o] = '\0';
  return buf;
}

int os_clipboard_set(const char *text, const size_t len) {
  FILE *pipe = popen(CLIPBOARD_COPY_CMD, "w");
  int ok;
  if (!pipe)
    return -1;
  ok = fwrite(text, 1, len, pipe) == len;
  return (pclose(pipe) == 0 && ok) ? 0 : -1;
}
