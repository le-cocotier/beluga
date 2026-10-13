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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h> // forkpty
#else
#include <pty.h> // forkpty (libutil on Linux)
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
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // Push the cursor to the bottom-right corner and ask where it is.
    os_term_write("\x1b[999C\x1b[999B", 12);
    return cursorPosition(rows, cols);
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
  p = calloc(1, sizeof(*p));
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

char **os_shell_argv(const char *command) {
  char **argv = calloc(4, sizeof(char *));
  if (!argv)
    return NULL;
  argv[0] = strdup("/bin/sh");
  argv[1] = strdup("-c");
  argv[2] = strdup(command);
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

OsPty *os_pty_open(const char *shell, const int rows, const int cols) {
  struct winsize ws;
  OsPty *p;
  pid_t pid;
  int master;
  const char *sh;

  p = calloc(1, sizeof(*p));
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
    // The child: become the shell.
    sh = (shell && *shell) ? shell : getenv("SHELL");
    if (!sh || !*sh)
      sh = access("/bin/zsh", X_OK) == 0 ? "/bin/zsh" : "/bin/sh";
    setenv("TERM", "xterm-256color", 1);
    execl(sh, sh, "-i", (char *)NULL);
    _exit(127);
  }

  setCloexec(master);
  p->pid = pid;
  streamInit(&p->out, master, 0);
  return p;
}

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
    list[n].name = strdup(entry->d_name);
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

char *os_realpath(const char *path) { return realpath(path, NULL); }

char *os_getcwd(void) {
  char buf[OS_PATH_MAX];
  return getcwd(buf, sizeof(buf)) ? strdup(buf) : NULL;
}

const char *os_home_dir(void) {
  const char *home = getenv("HOME");
  return (home && *home) ? home : "/";
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
  char *rs = realpath(src, NULL);
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
  rp = realpath(parent, NULL);
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
