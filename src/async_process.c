/**
 * @file async_process.c
 * @brief Programs run in the background, their output collected by the
 * main loop's select(), and a Lisp callback called when they exit.
 * @details The asynchronous counterpart of runArgvCapture() (subprocess.c):
 * same "no shell, stdout+stderr together, stdin from /dev/null" contract,
 * but instead of waiting it hands the pipe to editorRunLoop(). Built for
 * git's network commands, which can take far longer than the editor can
 * afford to freeze for.
 */

#include "../include/async_process.h"
#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/data.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern EditorConfig E;

#define MAX_ASYNC_PROCESSES 16
#define ASYNC_OUTPUT_CAP (1 << 20) // kept per process; the rest is drained and dropped

typedef struct {
  int in_use;
  pid_t pid;
  int fd;          // read end of the child's stdout+stderr pipe
  char *out;       // collected output, NUL-terminated
  size_t len;
  size_t cap;
  Lisp callback;
} AsyncProcess;

static AsyncProcess s_procs[MAX_ASYNC_PROCESSES];

int asyncProcessStart(char *const argv[], const Lisp callback) {
  int slot;
  int pipefd[2];
  pid_t pid;
  int devnull;

  if (!argv || !argv[0])
    return -1;
  for (slot = 0; slot < MAX_ASYNC_PROCESSES; slot++)
    if (!s_procs[slot].in_use)
      break;
  if (slot == MAX_ASYNC_PROCESSES)
    return -1;
  if (pipe(pipefd) != 0)
    return -1;

  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    // A session of its own: no controlling terminal, so ssh/git/gpg can't
    // open /dev/tty to prompt — they fail and say so in their output
    // instead of drawing over (and reading keys from) the editor.
    setsid();
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

  s_procs[slot].in_use = 1;
  s_procs[slot].pid = pid;
  s_procs[slot].fd = pipefd[0];
  s_procs[slot].cap = 256;
  s_procs[slot].len = 0;
  s_procs[slot].out = galloc(s_procs[slot].cap);
  if (s_procs[slot].out)
    s_procs[slot].out[0] = '\0';
  s_procs[slot].callback = callback;
  return slot;
}

int asyncProcessAddFds(fd_set *set, int maxfd) {
  int i;
  for (i = 0; i < MAX_ASYNC_PROCESSES; i++) {
    if (!s_procs[i].in_use)
      continue;
    FD_SET(s_procs[i].fd, set);
    if (s_procs[i].fd > maxfd)
      maxfd = s_procs[i].fd;
  }
  return maxfd;
}

int asyncProcessCount(void) {
  int i;
  int count = 0;
  for (i = 0; i < MAX_ASYNC_PROCESSES; i++)
    if (s_procs[i].in_use)
      count++;
  return count;
}

/**
 * @brief Appends a chunk to a process's collected output, up to the cap.
 */
static void appendOutput(AsyncProcess *p, const char *chunk, size_t n) {
  char *grown;
  if (!p->out)
    return;
  if (p->len + n > ASYNC_OUTPUT_CAP)
    n = p->len < ASYNC_OUTPUT_CAP ? ASYNC_OUTPUT_CAP - p->len : 0;
  if (n == 0)
    return;
  if (p->len + n + 1 > p->cap) {
    while (p->len + n + 1 > p->cap)
      p->cap *= 2;
    grown = grealloc(p->out, p->cap);
    if (!grown)
      return;
    p->out = grown;
  }
  memcpy(p->out + p->len, chunk, n);
  p->len += n;
  p->out[p->len] = '\0';
}

/**
 * @brief Reaps a process whose output hit EOF and calls its callback.
 * @details The slot is released *before* the callback runs, so a callback
 * that starts another process (fetch, then refresh, ...) can reuse it.
 */
static void finish(AsyncProcess *p) {
  int status;
  int exit_code = -1;
  Lisp callback;
  Lisp result;
  LispError error = LISP_ERROR_NONE;

  close(p->fd);
  if (waitpid(p->pid, &status, 0) == p->pid && WIFEXITED(status))
    exit_code = WEXITSTATUS(status);

  while (p->len > 0 && (p->out[p->len - 1] == '\n' || p->out[p->len - 1] == '\r'))
    p->out[--p->len] = '\0';

  result = lisp_cons(lisp_make_int(exit_code),
                     lisp_make_string2(p->out ? p->out : "", E.ctx), E.ctx);
  callback = p->callback;
  gfree(p->out);
  memset(p, 0, sizeof(*p));

  lisp_apply(callback, lisp_cons(result, lisp_null(), E.ctx), &error, E.ctx);
  if (error != LISP_ERROR_NONE)
    appDebug("[ASYNC] process callback raised an error\n");
}

void asyncProcessPoll(const fd_set *set) {
  int i;
  char chunk[4096];
  ssize_t n;
  AsyncProcess *p;

  for (i = 0; i < MAX_ASYNC_PROCESSES; i++) {
    p = &s_procs[i];
    if (!p->in_use || !FD_ISSET(p->fd, set))
      continue;
    for (;;) {
      n = read(p->fd, chunk, sizeof(chunk));
      if (n > 0) {
        appendOutput(p, chunk, (size_t) n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break; // drained for now; more will come
      if (n < 0 && errno == EINTR)
        continue;
      finish(p); // EOF (or a read error): the program is done writing
      break;
    }
  }
}
