/**
 * @file async_process.c
 * @brief Programs run in the background, their output collected by the
 * main loop, and a Lisp callback called when they exit.
 * @details The asynchronous counterpart of runArgvCapture() (subprocess.c):
 * same "no shell, stdout+stderr together, stdin empty" contract, but
 * instead of waiting it hands the output stream to editorRunLoop(). Built for
 * git's network commands, which can take far longer than the editor can
 * afford to freeze for.
 */

#include "../include/async_process.h"
#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/data.h"

#include <string.h>

extern EditorConfig E;

#define MAX_ASYNC_PROCESSES 16
#define ASYNC_OUTPUT_CAP (1 << 20) // kept per process; the rest is drained and dropped

typedef struct {
  int in_use;
  OsProc *proc;
  char *out;       // collected output, NUL-terminated
  size_t len;
  size_t cap;
  Lisp callback;
} AsyncProcess;

static AsyncProcess s_procs[MAX_ASYNC_PROCESSES];

int asyncProcessStart(char *const argv[], const Lisp callback) {
  OsSpawn spawn;
  int slot;

  if (!argv || !argv[0])
    return -1;
  for (slot = 0; slot < MAX_ASYNC_PROCESSES; slot++)
    if (!s_procs[slot].in_use)
      break;
  if (slot == MAX_ASYNC_PROCESSES)
    return -1;

  // No shell, stdout+stderr together, stdin empty, and no way to prompt:
  // a password prompt fails and says so in the output instead of drawing
  // over (and reading keys from) the editor.
  memset(&spawn, 0, sizeof(spawn));
  spawn.argv = (const char *const *)argv;
  spawn.stderr_mode = OS_STDERR_MERGE;
  spawn.no_prompt = 1;
  s_procs[slot].proc = os_proc_spawn(&spawn);
  if (!s_procs[slot].proc)
    return -1;

  s_procs[slot].in_use = 1;
  s_procs[slot].cap = 256;
  s_procs[slot].len = 0;
  s_procs[slot].out = galloc(s_procs[slot].cap);
  if (s_procs[slot].out)
    s_procs[slot].out[0] = '\0';
  s_procs[slot].callback = callback;
  return slot;
}

int asyncProcessStreams(OsStream **streams, const int max) {
  int i;
  int n = 0;
  for (i = 0; i < MAX_ASYNC_PROCESSES && n < max; i++)
    if (s_procs[i].in_use)
      streams[n++] = os_proc_output(s_procs[i].proc);
  return n;
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
  int exit_code = -1;
  Lisp callback;
  Lisp result;
  LispError error = LISP_ERROR_NONE;

  os_proc_wait(p->proc, -1, &exit_code);
  os_proc_free(p->proc);

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

void asyncProcessVisitLispRoots(LispRootVisitor visit, LispContext ctx) {
  int i;
  for (i = 0; i < MAX_ASYNC_PROCESSES; i++)
    if (s_procs[i].in_use)
      visit(&s_procs[i].callback, ctx);
}

void asyncProcessPoll(void) {
  int i;
  char chunk[4096];
  long n;
  AsyncProcess *p;

  for (i = 0; i < MAX_ASYNC_PROCESSES; i++) {
    p = &s_procs[i];
    if (!p->in_use)
      continue;
    for (;;) {
      n = os_stream_read(os_proc_output(p->proc), chunk, sizeof(chunk), 0);
      if (n > 0) {
        appendOutput(p, chunk, (size_t) n);
        continue;
      }
      if (n < 0)
        finish(p); // EOF: the program is done writing
      break;       // 0: drained for now; more will come
    }
  }
}
