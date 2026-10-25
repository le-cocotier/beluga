/**
 * @file dap.c
 * @brief The debugger: a Debug Adapter Protocol client
 * @details A session goes: start the adapter, initialize, launch; on the
 * adapter's "initialized" event send every breakpoint and, once they're
 * answered, configurationDone; then follow its events.
 *
 * When launch goes out depends on the adapter. Most (lldb-dap, debugpy)
 * only send "initialized" after launch, and keep the program waiting
 * until configurationDone. gdb sends it straight after initialize, and
 * its launch runs the program at once, so breakpoints sent after launch
 * race with the program. Hence launch waits a moment for "initialized":
 * if it comes, launch follows configurationDone; if not, it goes first.
 * gdb 16 refuses a configurationDone that comes before launch, so then
 * it goes again after launch. gdb also only reads the program's symbols
 * on launch, so it is given the program on its command line as well
 * (gdbArgv()): the breakpoints sent before launch then resolve at once
 * instead of coming back pending with "No source file named ...". A
 * "stopped" event asks for
 * the call stack, the selected frame's scopes and their variables, and
 * each answer refreshes the panel (debug_ui.c). Variables deeper down are
 * fetched when the panel expands them.
 *
 * Answers can arrive after the state they were asked about is gone (the
 * program was continued, another frame selected): every request that
 * fills the stack or the variables carries the generation it was sent
 * in, and s->gen moves on whenever that state is thrown away, so a late
 * answer is dropped instead of landing in freed memory.
 */

#include "../include/dap.h"

#include "../include/breakpoints.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/cJSON.h"
#include "../include/data.h"
#include "../include/debug_ui.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/jsonrpc_stream.h"
#include "../include/project.h"
#include "../include/pty_session.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

#define DAP_MAX_ADAPTERS 32
#define DAP_MAX_ARGS 8
#define DAP_MAX_PENDING 64
#define DAP_STACK_LEVELS 64
#define DAP_OUTPUT_LINES 2000
#define DAP_MAX_SESSIONS 8

typedef void (*DapHandler)(cJSON *response, void *ctx);

typedef struct
{
    int seq;
    DapHandler handler;
    void *ctx; // galloc'd, freed after the handler runs (or with the session)
} DapPending;

typedef struct
{
    char ext[16];
    char *argv[DAP_MAX_ARGS + 1];
    char *launch_defaults; // JSON object, or NULL
} DapAdapter;

// What a request filling the variables tree needs to find its node again.
typedef struct
{
    int gen;
    DapVar *var;
} VarRequest;

static DapAdapter s_adapters[DAP_MAX_ADAPTERS];
static int s_adapter_count;
// The watch expressions, evaluated whenever a session stops.
static char **s_watches;
static int s_watch_count;
// The exception filters chosen (dapSetExceptionFilters()), separated by
// spaces, or NULL for each adapter's defaults.
static char *s_exception_choice;

// One debug session per project (its root, or the working directory
// without one): in daemon mode each project can be debugged at once, each
// from the window that started it.
typedef struct
{
    // The adapter's process (NULL for one reached at tcp:HOST:PORT) and,
    // for an adapter that listens on a port, the connection to it. The
    // protocol goes over `conn` when there is one, else over the
    // process's stdin/stdout.
    OsProc *proc;
    OsConn *conn;
    char tcp_host[256];
    int tcp_port;              // 0: stdio
    long long connect_until;   // still trying to connect (ms), 0 otherwise
    char adapter_id[16];       // for initialize, sent once connected
    JsonRpcStream recv;
    int seq;
    DapState state;
    char adapter[64];
    // The project it debugs (or the working directory): only breakpoints
    // in files under it are sent, not every project's kept ones.
    char *root;
    cJSON *launch;         // launch (or attach) arguments, until the request is sent
    int attach;            // attached to a running process: stopping detaches
    long long launch_at;   // when launch goes anyway (ms), 0 before initialize
    int configured;        // breakpoints sent: changes go out as they happen
    // setBreakpoints still unanswered before configurationDone may go:
    // gdb lets the program run without them otherwise. -1 once it's sent.
    int config_waiting;
    // configurationDone went out before launch: gdb 16 refuses it then
    // and holds the program until it comes again.
    int config_early;
    // After "terminated": when disconnect goes out, then when the session
    // ends if the adapter hasn't exited by then (ms), 0 otherwise.
    long long ending_at;
    int disconnected;      // disconnect sent after "terminated"
    int thread_id;         // the thread that stopped (or the first one seen)
    int gen;
    DapPending pending[DAP_MAX_PENDING];
    int pending_count;
    DapFrame *frames;
    int frame_count;
    int selected;
    DapVar *scopes;
    int scope_count;
    char stop_reason[64];
    int exit_code;
    // Output, oldest line first; a line still being written waits in
    // partial. Kept after the session ends, until the next one starts.
    char *output[DAP_OUTPUT_LINES];
    int output_count;
    char *partial;
    // The project it was last started for: its output and exit code stay
    // that project's after the session ends.
    char last_root[OS_PATH_MAX];
    int owner; // the daemon window it reports to (dapSetWindowHooks()), 0: none
    cJSON *capabilities; // the adapter's, from initialize; NULL before
    // What the watch expressions came to in the selected frame, one per
    // expression (NULL: not answered yet); watch_value_count 0 while not
    // stopped.
    char **watch_values;
    int watch_value_count;
} DapSession;

static DapSession s_sessions[DAP_MAX_SESSIONS];
// The session being acted on: the current project's for commands and the
// panel (pickCurrent()), each in turn while the run loop reads them.
static DapSession *s = &s_sessions[0];

static int (*s_window_current)(void);
static int (*s_window_enter)(int id);
static void (*s_window_leave)(void);

/**
 * @brief The current project's session (its root, or the working
 * directory): the one running for it, else the last one that ran for it
 * (its output stays readable), else a free one.
 */
static DapSession *currentSession(void) {
  static int initialised;
  char *root = projectRootOrCwd();
  DapSession *found = NULL;
  DapSession *last = NULL;
  DapSession *blank = NULL;
  DapSession *idle = NULL;
  DapSession *x;
  int i;
  if (!initialised) {
    for (i = 0; i < DAP_MAX_SESSIONS; i++)
      s_sessions[i].exit_code = -1;
    initialised = 1;
  }
  for (i = 0; i < DAP_MAX_SESSIONS && !found; i++) {
    x = &s_sessions[i];
    if (x->state != DAP_OFF) {
      if (root && x->root && os_path_equal(x->root, root))
        found = x;
    } else if (root && x->last_root[0] && os_path_equal(x->last_root, root)) {
      last = x;
    } else if (!x->last_root[0] && !blank) {
      blank = x;
    } else if (!idle) {
      idle = x;
    }
  }
  gfree(root);
  if (found)
    return found;
  if (last)
    return last;
  if (blank)
    return blank;
  // Every slot is taken: an idle one is reused, else the first (which is
  // running, so dapStart() refuses).
  return idle ? idle : &s_sessions[0];
}

// currentSession(), for a command typed in a window: the session reports to
// that window from now on (daemon mode: the window that started it may be
// gone, or the user moved to another one).
static DapSession *drivenSession(void) {
  DapSession *x = currentSession();
  if (x->state != DAP_OFF && s_window_current)
    x->owner = s_window_current();
  return x;
}

// Makes `x` the session acted on, and its window the current one (daemon
// mode, when it still exists). leave() goes back.
static int enter(DapSession *x) {
  s = x;
  return x->owner && s_window_enter ? s_window_enter(x->owner) : 0;
}

static void leave(const int entered) {
  if (entered && s_window_leave)
    s_window_leave();
}

void dapSetWindowHooks(int (*current)(void), int (*enter_window)(int id), void (*leave_window)(void)) {
  s_window_current = current;
  s_window_enter = enter_window;
  s_window_leave = leave_window;
}

/***********************************************************************************************************************
 *
 *                                                 Output
 *
 **********************************************************************************************************************/

static void outputPushLine(const char *text, const size_t len) {
  char *line = utilStrndup(text, len);
  if (!line)
    return;
  if (s->output_count == DAP_OUTPUT_LINES) {
    gfree(s->output[0]);
    memmove(s->output, s->output + 1, sizeof(char *) * (DAP_OUTPUT_LINES - 1));
    s->output_count--;
  }
  s->output[s->output_count++] = line;
}

static void outputFlushPartial(void) {
  if (s->partial && *s->partial)
    outputPushLine(s->partial, strlen(s->partial));
  gfree(s->partial);
  s->partial = NULL;
}

/**
 * @brief Appends `text` to the output: whole lines go in, the rest waits
 * for the line's end. Carriage returns are dropped.
 */
static void outputAppend(const char *text) {
  char *joined;
  char *p;
  char *start;
  char *nl;
  joined = utilFormat("%s%s", s->partial ? s->partial : "", text);
  gfree(s->partial);
  s->partial = NULL;
  if (!joined)
    return;
  for (p = start = joined; *p; p++)
    if (*p != '\r')
      *start++ = *p;
  *start = '\0';
  start = joined;
  while ((nl = strchr(start, '\n')) != NULL) {
    outputPushLine(start, (size_t)(nl - start));
    start = nl + 1;
  }
  if (*start)
    s->partial = utilStrdup(start);
  gfree(joined);
}

static void outputClear(void) {
  int i;
  for (i = 0; i < s->output_count; i++)
    gfree(s->output[i]);
  s->output_count = 0;
  gfree(s->partial);
  s->partial = NULL;
}

const char *const *dapOutputTail(int *count) {
  DapSession *c = currentSession();
  *count = c->output_count;
  return (const char *const *)c->output;
}

/***********************************************************************************************************************
 *
 *                                                 Stack and variables
 *
 **********************************************************************************************************************/

static void freeVars(DapVar *vars, const int count) {
  int i;
  for (i = 0; i < count; i++) {
    gfree(vars[i].name);
    gfree(vars[i].value);
    gfree(vars[i].type);
    freeVars(vars[i].children, vars[i].child_count);
  }
  gfree(vars);
}

static void clearScopes(void) {
  freeVars(s->scopes, s->scope_count);
  s->scopes = NULL;
  s->scope_count = 0;
}

static void clearFrames(void) {
  int i;
  for (i = 0; i < s->frame_count; i++) {
    gfree(s->frames[i].name);
    gfree(s->frames[i].path);
  }
  gfree(s->frames);
  s->frames = NULL;
  s->frame_count = 0;
  s->selected = 0;
}

static void clearWatchValues(void) {
  int i;
  for (i = 0; i < s->watch_value_count; i++)
    gfree(s->watch_values[i]);
  gfree(s->watch_values);
  s->watch_values = NULL;
  s->watch_value_count = 0;
}

// The stack and variables no longer describe the program: drop them, and
// any answer still on its way about them.
static void forgetStoppedState(void) {
  s->gen++;
  clearScopes();
  clearFrames();
  clearWatchValues();
}

const DapFrame *dapFrames(int *count, int *selected) {
  DapSession *c = currentSession();
  *count = c->frame_count;
  *selected = c->selected;
  return c->frames;
}

DapVar *dapScopes(int *count) {
  DapSession *c = currentSession();
  *count = c->scope_count;
  return c->scopes;
}

int dapCurrentLine(const char **path, int *line) {
  DapSession *c = currentSession();
  if (c->state != DAP_STOPPED || c->selected >= c->frame_count || !c->frames[c->selected].path)
    return 0;
  *path = c->frames[c->selected].path;
  *line = c->frames[c->selected].line;
  return 1;
}

DapState dapState(void) { return currentSession()->state; }
const char *dapStopReason(void) { return currentSession()->stop_reason; }
const char *dapAdapterName(void) { return currentSession()->adapter; }
int dapExitCode(void) { return currentSession()->exit_code; }

// Session `x`'s protocol stream, when there is one to read.
static OsStream *sessionStream(DapSession *x) {
  if (x->conn)
    return os_conn_input(x->conn);
  return x->proc && !x->tcp_port ? os_proc_output(x->proc) : NULL;
}

int dapStreams(OsStream **streams, const int max) {
  int n = 0;
  int i;
  for (i = 0; i < DAP_MAX_SESSIONS && n < max; i++)
    if (sessionStream(&s_sessions[i]))
      streams[n++] = sessionStream(&s_sessions[i]);
  return n;
}

// A session is open and its adapter can be talked to.
static int connected(void) { return s->conn || (s->proc && !s->tcp_port); }

/***********************************************************************************************************************
 *
 *                                                 Transport
 *
 **********************************************************************************************************************/

static void clearPending(void) {
  int i;
  for (i = 0; i < s->pending_count; i++)
    gfree(s->pending[i].ctx);
  s->pending_count = 0;
}

/**
 * @brief Sends request `command` with `arguments` (taken over; NULL for
 * none). `handler` gets the response, then `ctx` is freed.
 * @return the request's seq, or -1 if there's no session.
 */
// Writes one message (Content-Length header, then the JSON).
static void dapWrite(const char *json) {
  char header[64];
  const size_t len = strlen(json);
  if (!s->conn) {
    jsonRpcWrite(s->proc, json, "DAP");
    return;
  }
  snprintf(header, sizeof(header), "Content-Length: %u\r\n\r\n", (unsigned)len);
  appDebug("[DAP →] %s\n", json);
  if (os_conn_write(s->conn, header, strlen(header)) == -1 || os_conn_write(s->conn, json, len) == -1)
    appDebug("[DAP] the connection to the adapter is gone\n");
}

static int sendRequest(const char *command, cJSON *arguments, const DapHandler handler,
                       void *ctx) {
  cJSON *msg;
  char *text;
  int seq;
  if (!connected()) {
    cJSON_Delete(arguments);
    gfree(ctx);
    return -1;
  }
  seq = ++s->seq;
  msg = cJSON_CreateObject();
  cJSON_AddNumberToObject(msg, "seq", seq);
  cJSON_AddStringToObject(msg, "type", "request");
  cJSON_AddStringToObject(msg, "command", command);
  if (arguments)
    cJSON_AddItemToObject(msg, "arguments", arguments);
  text = cJSON_PrintUnformatted(msg);
  cJSON_Delete(msg);
  if (text) {
    dapWrite(text);
    gfree(text);
  }
  if (handler && s->pending_count < DAP_MAX_PENDING) {
    s->pending[s->pending_count].seq = seq;
    s->pending[s->pending_count].handler = handler;
    s->pending[s->pending_count].ctx = ctx;
    s->pending_count++;
  } else {
    gfree(ctx);
  }
  return seq;
}

static cJSON *threadArguments(void) {
  cJSON *args = cJSON_CreateObject();
  cJSON_AddNumberToObject(args, "threadId", s->thread_id);
  return args;
}

static int responseOk(cJSON *response) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "success"));
}

static const char *responseMessage(cJSON *response) {
  const char *m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "message"));
  cJSON *body = cJSON_GetObjectItemCaseSensitive(response, "body");
  cJSON *error = cJSON_GetObjectItemCaseSensitive(body, "error");
  const char *format = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "format"));
  return format ? format : (m ? m : "failed");
}

/***********************************************************************************************************************
 *
 *                                                 Ending
 *
 **********************************************************************************************************************/

/**
 * @brief Tears the session down: the adapter (killed if it's still
 * running), the requests in flight, the stack. The output stays, to be
 * read after the program is gone.
 */
static void endSession(const char *message, const int run_hook) {
  const int had_session = s->state != DAP_OFF;
  outputFlushPartial();
  forgetStoppedState();
  clearPending();
  // s->recv stays: this may run from inside dapFeed(), which is still
  // reading it. It's emptied when the next session starts.
  cJSON_Delete(s->launch);
  s->launch = NULL;
  gfree(s->root);
  s->root = NULL;
  if (s->conn) {
    os_conn_close(s->conn);
    s->conn = NULL;
  }
  s->tcp_port = 0;
  s->connect_until = 0;
  if (s->proc) {
    os_proc_free(s->proc);
    s->proc = NULL;
  }
  cJSON_Delete(s->capabilities);
  s->capabilities = NULL;
  s->state = DAP_OFF;
  s->configured = 0;
  s->ending_at = 0;
  s->disconnected = 0;
  if (message)
    editorSetStatusMessage("%s", message);
  debugUiRefresh();
  if (had_session && run_hook && E.initialized)
    runHooks("debug-exited", lisp_cons(lisp_make_int(s->exit_code), lisp_null(), E.ctx));
}

/**
 * @brief Asks the adapter to end the program and itself, then ends the
 * session.
 */
static void stopSession(const int run_hook) {
  cJSON *args;
  if (s->state == DAP_OFF)
    return;
  // A program it attached to goes on running on its own.
  args = cJSON_CreateObject();
  cJSON_AddBoolToObject(args, "terminateDebuggee", !s->attach);
  sendRequest("disconnect", args, NULL, NULL);
  // Give it a moment to end the program and exit on its own;
  // os_proc_free() kills whatever is left.
  if (s->proc)
    os_proc_wait(s->proc, 1000, NULL);
  endSession(s->attach ? "Detached from the program" : "Debug session ended", run_hook);
}

void dapStop(void) {
  DapSession *saved = s;
  s = drivenSession();
  stopSession(1);
  s = saved;
}

/***********************************************************************************************************************
 *
 *                                                 Breakpoints
 *
 **********************************************************************************************************************/

static void onLaunch(cJSON *response, void *ctx);

static void sendLaunch(void) {
  cJSON *launch = s->launch;
  s->launch = NULL;
  if (launch)
    sendRequest(s->attach ? "attach" : "launch", launch, onLaunch, NULL);
}

static void onConfigurationDone(cJSON *response, void *ctx) {
  (void)ctx;
  // gdb 15 and earlier take configurationDone before launch and run the
  // program on launch; gdb 16 answers "launch or attach not specified"
  // and only runs it on a configurationDone after launch, which has gone
  // out by now.
  if (s->config_early && !responseOk(response) && s->state != DAP_OFF)
    sendRequest("configurationDone", NULL, NULL, NULL);
  s->config_early = 0;
}

// The program may run once the adapter has every breakpoint.
static void configurationDone(void) {
  s->config_waiting = -1;
  s->config_early = s->launch != NULL;
  sendRequest("configurationDone", NULL, onConfigurationDone, NULL);
  sendLaunch(); // if it waited for this
  if (s->state == DAP_STARTING)
    s->state = DAP_RUNNING;
}

static void onSetBreakpoints(cJSON *response, void *ctx) {
  const char *path = ctx;
  cJSON *answers = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(response, "body"), "breakpoints");
  cJSON *answer;
  Breakpoint *all;
  int count;
  int i;
  if (s->config_waiting > 0 && --s->config_waiting == 0)
    configurationDone();
  if (!responseOk(response)) {
    editorSetStatusMessage("Breakpoints in %s: %s", path, responseMessage(response));
    return;
  }
  // The answers come in the order the breakpoints were sent: the file's,
  // by line. If the set changed since, a newer request is on its way.
  if (cJSON_GetArraySize(answers) != breakpointsInFile(path))
    return;
  all = breakpointsAll(&count);
  answer = answers ? answers->child : NULL;
  for (i = 0; i < count && answer; i++) {
    if (!os_path_equal(all[i].path, path))
      continue;
    all[i].verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "verified"));
    all[i].id = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(answer, "id"));
    answer = answer->next;
  }
}

static void sendBreakpoints(const char *path) {
  Breakpoint *all;
  int count;
  int i;
  cJSON *args;
  cJSON *source;
  cJSON *list;
  cJSON *bp;
  const char *name;
  all = breakpointsAll(&count);
  args = cJSON_CreateObject();
  source = cJSON_AddObjectToObject(args, "source");
  name = strrchr(path, '/');
  cJSON_AddStringToObject(source, "name", name ? name + 1 : path);
  cJSON_AddStringToObject(source, "path", path);
  list = cJSON_AddArrayToObject(args, "breakpoints");
  for (i = 0; i < count; i++) {
    if (!os_path_equal(all[i].path, path))
      continue;
    all[i].verified = 0;
    bp = cJSON_CreateObject();
    cJSON_AddNumberToObject(bp, "line", all[i].line + 1);
    if (all[i].condition)
      cJSON_AddStringToObject(bp, "condition", all[i].condition);
    if (all[i].hit_condition)
      cJSON_AddStringToObject(bp, "hitCondition", all[i].hit_condition);
    if (all[i].log_message)
      cJSON_AddStringToObject(bp, "logMessage", all[i].log_message);
    cJSON_AddItemToArray(list, bp);
  }
  sendRequest("setBreakpoints", args, onSetBreakpoints, utilStrdup(path));
}

// Every file's breakpoints, one setBreakpoints each. @return how many.
static int sendAllBreakpoints(void) {
  Breakpoint *all;
  int count;
  int i;
  int sent = 0;
  all = breakpointsAll(&count);
  for (i = 0; i < count; i++)
    if ((i == 0 || strcmp(all[i].path, all[i - 1].path) != 0) &&
        (!s->root || utilPathIsUnder(all[i].path, s->root))) {
      sendBreakpoints(all[i].path);
      sent++;
    }
  return sent;
}

// breakpoints.c's listener: a file's breakpoints changed.
static void onBreakpointsChanged(const char *path) {
  DapSession *saved = s;
  int i;
  for (i = 0; i < DAP_MAX_SESSIONS; i++) {
    s = &s_sessions[i];
    if (connected() && s->configured && (!s->root || utilPathIsUnder(path, s->root)))
      sendBreakpoints(path);
  }
  s = saved;
}

static void onBreakpointEvent(cJSON *body) {
  cJSON *bp = cJSON_GetObjectItemCaseSensitive(body, "breakpoint");
  const int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(bp, "id"));
  Breakpoint *all;
  int count;
  int i;
  if (id <= 0)
    return;
  all = breakpointsAll(&count);
  for (i = 0; i < count; i++)
    if (all[i].id == id)
      all[i].verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(bp, "verified"));
}

/***********************************************************************************************************************
 *
 *                                                 Exceptions
 *
 **********************************************************************************************************************/

// The adapter's exception filters (an array of {filter, label, default}),
// or NULL when it has none.
static cJSON *exceptionFilters(void) {
  cJSON *list = cJSON_GetObjectItemCaseSensitive(s->capabilities, "exceptionBreakpointFilters");
  return cJSON_GetArraySize(list) > 0 ? list : NULL;
}

// Is `id` in the space-separated `ids`?
static int wordIn(const char *id, const char *ids) {
  const size_t len = strlen(id);
  const char *p = ids;
  while (p && (p = strstr(p, id)) != NULL) {
    if ((p == ids || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0'))
      return 1;
    p += len;
  }
  return 0;
}

// Is the adapter's `filter` on: chosen, or by default when nothing is?
static int filterOn(cJSON *filter) {
  const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(filter, "filter"));
  if (!id)
    return 0;
  if (s_exception_choice)
    return wordIn(id, s_exception_choice);
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(filter, "default"));
}

static void onSetExceptionBreakpoints(cJSON *response, void *ctx) {
  (void)ctx;
  if (s->config_waiting > 0 && --s->config_waiting == 0)
    configurationDone();
  if (!responseOk(response))
    editorSetStatusMessage("Exception breakpoints: %s", responseMessage(response));
}

// Tells the adapter which exceptions stop the program: the chosen filters
// it knows. @return 1 if it was sent (the adapter has filters), else 0.
static int sendExceptionBreakpoints(void) {
  cJSON *filters = exceptionFilters();
  cJSON *filter;
  cJSON *args;
  cJSON *list;
  if (!filters)
    return 0;
  args = cJSON_CreateObject();
  list = cJSON_AddArrayToObject(args, "filters");
  cJSON_ArrayForEach(filter, filters) {
    if (filterOn(filter))
      cJSON_AddItemToArray(list, cJSON_CreateString(cJSON_GetStringValue(
                                     cJSON_GetObjectItemCaseSensitive(filter, "filter"))));
  }
  sendRequest("setExceptionBreakpoints", args, onSetExceptionBreakpoints, NULL);
  return 1;
}

char *dapExceptionFilters(int *count) {
  DapSession *saved = s;
  cJSON *filter;
  char *text = NULL;
  char *joined;
  const char *id;
  const char *label;
  *count = 0;
  s = currentSession();
  cJSON_ArrayForEach(filter, exceptionFilters()) {
    id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(filter, "filter"));
    label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(filter, "label"));
    if (!id)
      continue;
    joined = utilFormat("%s%s%s%s (%s)", text ? text : "", text ? ", " : "", filterOn(filter) ? "*" : "",
                        id, label ? label : id);
    gfree(text);
    text = joined;
    (*count)++;
  }
  s = saved;
  return text;
}

char *dapExceptionChoice(void) {
  DapSession *saved = s;
  cJSON *filter;
  char *text = NULL;
  char *joined;
  if (s_exception_choice)
    return utilStrdup(s_exception_choice);
  s = currentSession();
  cJSON_ArrayForEach(filter, exceptionFilters()) {
    if (!filterOn(filter))
      continue;
    joined = utilFormat("%s%s%s", text ? text : "", text ? " " : "",
                        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(filter, "filter")));
    gfree(text);
    text = joined;
  }
  s = saved;
  return text ? text : utilStrdup("");
}

void dapSetExceptionFilters(const char *ids) {
  DapSession *saved = s;
  int i;
  gfree(s_exception_choice);
  s_exception_choice = ids ? utilStrdup(ids) : NULL;
  for (i = 0; i < DAP_MAX_SESSIONS; i++) {
    s = &s_sessions[i];
    if (connected() && s->configured)
      sendExceptionBreakpoints();
  }
  s = saved;
}

/***********************************************************************************************************************
 *
 *                                                 Responses
 *
 **********************************************************************************************************************/

static char *jsonString(cJSON *object, const char *key) {
  const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
  return v ? utilStrdup(v) : NULL;
}

static void onVariables(cJSON *response, void *ctx) {
  const VarRequest *req = ctx;
  cJSON *list;
  cJSON *item;
  DapVar *var;
  DapVar *children;
  int n;
  int i;
  if (req->gen != s->gen)
    return; // about a stop that's gone
  var = req->var;
  var->loaded = 1;
  if (!responseOk(response)) {
    debugUiRefresh();
    return;
  }
  list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "body"),
                                          "variables");
  n = cJSON_GetArraySize(list);
  children = n > 0 ? gcalloc((size_t)n, sizeof(DapVar)) : NULL;
  if (n > 0 && !children)
    return;
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (i >= n)
      break;
    children[i].name = jsonString(item, "name");
    children[i].value = jsonString(item, "value");
    children[i].type = jsonString(item, "type");
    children[i].ref = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItemCaseSensitive(item, "variablesReference"));
    i++;
  }
  freeVars(var->children, var->child_count);
  var->children = children;
  var->child_count = i;
  debugUiRefresh();
}

static void requestVariables(DapVar *var) {
  cJSON *args;
  VarRequest *req = galloc(sizeof(VarRequest));
  if (!req)
    return;
  req->gen = s->gen;
  req->var = var;
  args = cJSON_CreateObject();
  cJSON_AddNumberToObject(args, "variablesReference", var->ref);
  sendRequest("variables", args, onVariables, req);
}

static int varInTree(const DapVar *var, const DapVar *vars, const int count) {
  int i;
  for (i = 0; i < count; i++)
    if (var == &vars[i] || varInTree(var, vars[i].children, vars[i].child_count))
      return 1;
  return 0;
}

void dapToggleVar(DapVar *var) {
  DapSession *saved = s;
  s = drivenSession();
  // The panel's row may be older than the tree; only a node still in it.
  if (var && varInTree(var, s->scopes, s->scope_count) && var->ref > 0) {
    var->expanded = !var->expanded;
    if (var->expanded && !var->loaded)
      requestVariables(var);
    debugUiRefresh();
  }
  s = saved;
}

static void onScopes(cJSON *response, void *ctx) {
  const int gen = *(const int *)ctx;
  cJSON *list;
  cJSON *item;
  const char *hint;
  int n;
  int i;
  if (gen != s->gen || !responseOk(response))
    return;
  list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "body"),
                                          "scopes");
  n = cJSON_GetArraySize(list);
  clearScopes();
  s->scopes = n > 0 ? gcalloc((size_t)n, sizeof(DapVar)) : NULL;
  if (!s->scopes) {
    debugUiRefresh();
    return;
  }
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (i >= n)
      break;
    s->scopes[i].name = jsonString(item, "name");
    s->scopes[i].ref = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItemCaseSensitive(item, "variablesReference"));
    i++;
  }
  s->scope_count = i;
  // Open the cheap ones (locals, arguments); "expensive" ones (globals)
  // and the registers wait to be opened from the panel.
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (i >= s->scope_count)
      break;
    hint = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "presentationHint"));
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "expensive")) &&
        !(hint && !strcmp(hint, "registers")) && s->scopes[i].ref > 0) {
      s->scopes[i].expanded = 1;
      requestVariables(&s->scopes[i]);
    }
    i++;
  }
  debugUiRefresh();
}

// What an evaluate request for a watch needs to find its place again.
typedef struct
{
    int gen;
    int index;
    // The watch it was for (the list may have changed since), stored after
    // the struct: one block, which the session frees if no answer comes.
    char *expression;
} WatchRequest;

static void onWatchEvaluate(cJSON *response, void *ctx) {
  WatchRequest *req = ctx;
  const char *result;
  if (req->gen == s->gen && req->index < s->watch_value_count && req->index < s_watch_count &&
      !strcmp(s_watches[req->index], req->expression)) {
    if (responseOk(response))
      result = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(response, "body"), "result"));
    else
      result = responseMessage(response);
    gfree(s->watch_values[req->index]);
    s->watch_values[req->index] = utilStrdup(result ? result : "");
    debugUiRefresh();
  }
}

// Asks for every watch expression's value in the selected frame.
static void evaluateWatches(void) {
  WatchRequest *req;
  cJSON *args;
  size_t len;
  int i;
  clearWatchValues();
  if (s->state != DAP_STOPPED || s_watch_count == 0)
    return;
  s->watch_values = gcalloc((size_t)s_watch_count, sizeof(char *));
  if (!s->watch_values)
    return;
  s->watch_value_count = s_watch_count;
  for (i = 0; i < s_watch_count; i++) {
    len = strlen(s_watches[i]);
    req = galloc(sizeof(WatchRequest) + len + 1);
    if (!req)
      continue;
    req->gen = s->gen;
    req->index = i;
    req->expression = (char *)(req + 1);
    memcpy(req->expression, s_watches[i], len + 1);
    args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "expression", s_watches[i]);
    cJSON_AddStringToObject(args, "context", "watch");
    if (s->selected < s->frame_count)
      cJSON_AddNumberToObject(args, "frameId", s->frames[s->selected].id);
    sendRequest("evaluate", args, onWatchEvaluate, req);
  }
}

// Evaluates the watches again in every stopped session.
static void reevaluateWatches(void) {
  DapSession *saved = s;
  int i;
  for (i = 0; i < DAP_MAX_SESSIONS; i++) {
    s = &s_sessions[i];
    if (s->state == DAP_STOPPED && connected())
      evaluateWatches();
  }
  s = saved;
  debugUiRefresh();
}

int dapWatchAdd(const char *expression) {
  char **grown;
  int i;
  if (!expression || !*expression)
    return -1;
  for (i = 0; i < s_watch_count; i++)
    if (!strcmp(s_watches[i], expression))
      return 0;
  grown = grealloc(s_watches, sizeof(char *) * (size_t)(s_watch_count + 1));
  if (!grown)
    return -1;
  s_watches = grown;
  s_watches[s_watch_count] = utilStrdup(expression);
  if (!s_watches[s_watch_count])
    return -1;
  s_watch_count++;
  reevaluateWatches();
  return 0;
}

int dapWatchRemove(const int index) {
  if (index < 0 || index >= s_watch_count)
    return -1;
  gfree(s_watches[index]);
  memmove(&s_watches[index], &s_watches[index + 1],
          sizeof(char *) * (size_t)(s_watch_count - index - 1));
  s_watch_count--;
  reevaluateWatches();
  return 0;
}

const char *const *dapWatches(int *count) {
  *count = s_watch_count;
  return (const char *const *)s_watches;
}

const char *dapWatchValue(const int index) {
  const DapSession *c = currentSession();
  return index >= 0 && index < c->watch_value_count ? c->watch_values[index] : NULL;
}

static void selectFrame(int index);

void dapSelectFrame(const int index) {
  DapSession *saved = s;
  s = drivenSession();
  selectFrame(index);
  s = saved;
}

static void selectFrame(const int index) {
  cJSON *args;
  int *gen;
  if (s->state != DAP_STOPPED || index < 0 || index >= s->frame_count)
    return;
  s->gen++;
  clearScopes();
  s->selected = index;
  if (s->frames[index].path)
    debugUiShowLocation(s->frames[index].path, s->frames[index].line);
  gen = galloc(sizeof(int));
  if (gen)
    *gen = s->gen;
  args = cJSON_CreateObject();
  cJSON_AddNumberToObject(args, "frameId", s->frames[index].id);
  sendRequest("scopes", args, onScopes, gen);
  evaluateWatches();
  debugUiRefresh();
}

static void onStackTrace(cJSON *response, void *ctx) {
  const int gen = *(const int *)ctx;
  cJSON *list;
  cJSON *item;
  cJSON *source;
  int n;
  int i;
  int first_with_source;
  const char *path;
  if (gen != s->gen)
    return;
  if (!responseOk(response)) {
    editorSetStatusMessage("Call stack: %s", responseMessage(response));
    return;
  }
  list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "body"),
                                          "stackFrames");
  n = cJSON_GetArraySize(list);
  clearFrames();
  s->frames = n > 0 ? gcalloc((size_t)n, sizeof(DapFrame)) : NULL;
  first_with_source = -1;
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (!s->frames || i >= n)
      break;
    s->frames[i].id = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
    s->frames[i].name = jsonString(item, "name");
    source = cJSON_GetObjectItemCaseSensitive(item, "source");
    // A source with a sourceReference only exists inside the adapter
    // (lldb-dap's disassembly, "/usr/lib/dyld`start"): no file to open.
    if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(source, "sourceReference")) > 0)
      s->frames[i].path = NULL;
    else
      s->frames[i].path = jsonString(source, "path");
    s->frames[i].line =
        (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "line")) - 1;
    if (s->frames[i].line < 0)
      s->frames[i].line = 0;
    if (first_with_source < 0 && s->frames[i].path)
      first_with_source = i;
    i++;
  }
  s->frame_count = i;
  // Stopped inside code without debug info (a library): show the first
  // frame that has a source file.
  selectFrame(first_with_source >= 0 ? first_with_source : 0);
  debugUiRefresh();

  if (E.initialized) {
    path = s->frame_count > 0 ? s->frames[s->selected].path : NULL;
    runHooks("debug-stopped",
             lisp_cons(path ? lisp_make_string2(path, E.ctx) : lisp_false(),
                       lisp_cons(lisp_make_int(s->frame_count > 0 ? s->frames[s->selected].line + 1 : 0),
                                 lisp_cons(lisp_make_string2(s->stop_reason, E.ctx), lisp_null(), E.ctx),
                                 E.ctx),
                       E.ctx));
  }
}

static void onLaunch(cJSON *response, void *ctx) {
  (void)ctx;
  if (!responseOk(response)) {
    outputAppend(s->attach ? "Attach failed: " : "Launch failed: ");
    outputAppend(responseMessage(response));
    outputAppend("\n");
    endSession(NULL, 1);
    editorSetStatusMessage(s->attach ? "Couldn't attach to the program: %s"
                                     : "Couldn't launch the program: %s",
                           responseMessage(response));
  }
}

#define LAUNCH_WAIT_MS 300 // for an "initialized" event before launch
#define DAP_CONNECT_TIMEOUT_MS 10000 // for an adapter to listen on its port

static void onInitialize(cJSON *response, void *ctx) {
  (void)ctx;
  if (!responseOk(response)) {
    endSession(NULL, 1);
    editorSetStatusMessage("The debug adapter refused to start: %s", responseMessage(response));
    return;
  }
  cJSON_Delete(s->capabilities);
  s->capabilities = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(response, "body"), 1);
  s->launch_at = os_monotonic_ms() + LAUNCH_WAIT_MS;
}

#define TERMINATED_WAIT_MS 300  // for the program's last output after "terminated"
#define DISCONNECT_WAIT_MS 1000 // for the adapter to exit after disconnect

// The program is gone: ends the session, saying how it exited.
static void endTerminated(void) {
  char message[64];
  snprintf(message, sizeof(message), "The program exited with code %d", s->exit_code);
  endSession(message, 1);
}

static void sendInitialize(void);

/**
 * @brief An adapter reached over TCP: what its process prints goes to the
 * output, and until it is connected, one more try each tick.
 */
static void tickTcp(void) {
  char chunk[1024];
  char err[300];
  char message[400];
  long n;
  if (s->proc && s->tcp_port) {
    while ((n = os_stream_read(os_proc_output(s->proc), chunk, sizeof(chunk) - 1, 0)) > 0) {
      chunk[n] = '\0';
      outputAppend(chunk);
    }
  }
  if (!s->connect_until)
    return;
  s->conn = os_tcp_connect(s->tcp_host, s->tcp_port, 20, err, sizeof(err));
  if (s->conn) {
    s->connect_until = 0;
    sendInitialize();
  } else if (os_monotonic_ms() >= s->connect_until ||
             (s->proc && os_proc_wait(s->proc, 0, NULL) == 1)) {
    snprintf(message, sizeof(message), "Couldn't connect to the debug adapter: %s", err);
    endSession(message, 1);
  }
}

static void tickSession(void) {
  tickTcp();
  if (s->launch && s->launch_at && os_monotonic_ms() >= s->launch_at)
    sendLaunch();
  if (s->ending_at && os_monotonic_ms() >= s->ending_at) {
    if (s->disconnected) {
      endTerminated();
    } else {
      sendRequest("disconnect", NULL, NULL, NULL);
      s->disconnected = 1;
      s->ending_at = os_monotonic_ms() + DISCONNECT_WAIT_MS;
    }
  }
}

void dapTick(void) {
  DapSession *saved = s;
  int entered;
  int i;
  for (i = 0; i < DAP_MAX_SESSIONS; i++) {
    if (s_sessions[i].state == DAP_OFF)
      continue;
    entered = enter(&s_sessions[i]);
    tickSession();
    leave(entered);
  }
  s = saved;
}

static void onEvaluate(cJSON *response, void *ctx) {
  const char *expression = ctx;
  const char *result;
  char *line;
  char first[256];
  size_t len;
  if (responseOk(response)) {
    result = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(response, "body"), "result"));
    result = result ? result : "";
  } else {
    result = responseMessage(response);
  }
  line = utilFormat("> %s\n%s\n", expression, result);
  if (line) {
    outputFlushPartial();
    outputAppend(line);
    gfree(line);
  }
  // The status bar has one line: lldb's errors come after a warning and
  // are followed by the expression, so the "error:" line if there is one.
  if (strstr(result, "error:"))
    result = strstr(result, "error:");
  len = strcspn(result, "\r\n");
  // (editorSetStatusMessage's own printf has no "%.*s".)
  snprintf(first, sizeof(first), "%.*s", (int)len, result);
  editorSetStatusMessage("%s = %s%s", expression, first,
                         result[len + strspn(result + len, "\r\n")] ? " (o: all of it)" : "");
  debugUiRefresh();
}

// Answers one of the adapter's own requests; `body` (taken over) may be
// NULL, `message` is for a refusal.
static void respond(cJSON *request, const int success, cJSON *body, const char *message) {
  cJSON *msg;
  char *text;
  cJSON *seq = cJSON_GetObjectItemCaseSensitive(request, "seq");
  const char *command = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "command"));
  msg = cJSON_CreateObject();
  cJSON_AddNumberToObject(msg, "seq", ++s->seq);
  cJSON_AddStringToObject(msg, "type", "response");
  cJSON_AddNumberToObject(msg, "request_seq", cJSON_GetNumberValue(seq));
  cJSON_AddStringToObject(msg, "command", command ? command : "");
  cJSON_AddBoolToObject(msg, "success", success);
  if (!success)
    cJSON_AddStringToObject(msg, "message", message ? message : "failed");
  if (body)
    cJSON_AddItemToObject(msg, "body", body);
  text = cJSON_PrintUnformatted(msg);
  cJSON_Delete(msg);
  if (text) {
    dapWrite(text);
    gfree(text);
  }
}

#define DEBUGGEE_BUFFER "*debuggee*"

/**
 * @brief Shows terminal buffer `id` in pane `pane` (the last program's)
 * or, with -1, in a new pane under the active one, and moves the focus
 * there: the program is what reads the keys now (ESC gives them back to
 * the editor). @return 0, or -1 when the pane is too small to split (the
 * buffer is then only reached with CTRL-x b).
 */
static int showProgramPane(const int id, int pane) {
  ScreenLayout *layout = splitScreenGetLayout();
  struct prefix_t prefix;
  int i;
  if (pane < 0 || pane >= layout->num_panes) {
    if (splitScreenSplitActive(SPLIT_HORIZONTAL) != 0)
      return -1;
    layout = splitScreenGetLayout();
    pane = layout->active_pane;
  }
  layout->panes[pane].buffer_id = id;
  layout->panes[pane].shown_buffer_id = -1;
  for (i = 0; i < layout->num_panes; i++)
    layout->panes[i].is_active = 0;
  layout->active_pane = pane;
  layout->panes[pane].is_active = 1;
  // The terminal's keymap, or ENTER, arrows... run editor commands (the
  // split above left the prefix of the pane it copied).
  prefix = find_prefix("term");
  if (prefix.prefix_id != -1)
    E.prefix_state = prefix.prefix_id;
  return 0;
}

/**
 * @brief runInTerminal: the program runs in a terminal pane of its own,
 * *debuggee*, so it can read keys and draw (curses), rather than through
 * the adapter. The last session's *debuggee* is closed first.
 */
static void runInTerminal(cJSON *request) {
  cJSON *args = cJSON_GetObjectItemCaseSensitive(request, "arguments");
  cJSON *argv_json = cJSON_GetObjectItemCaseSensitive(args, "args");
  cJSON *env_json = cJSON_GetObjectItemCaseSensitive(args, "env");
  const char *cwd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "cwd"));
  const char **argv = NULL;
  char **env = NULL;
  cJSON *item;
  cJSON *body;
  Utf8Char *name;
  struct buffer_t *old;
  int n_args = cJSON_GetArraySize(argv_json);
  int n_env = cJSON_GetArraySize(env_json);
  ScreenLayout *layout;
  int pane = -1;
  int id;
  int i = 0;
  long pid = -1;

  argv = gcalloc((size_t)n_args + 1, sizeof(*argv));
  env = gcalloc((size_t)n_env + 1, sizeof(*env));
  if (!argv || !env || n_args == 0)
    goto done;
  cJSON_ArrayForEach(item, argv_json) {
    if (!cJSON_IsString(item))
      goto done;
    argv[i++] = item->valuestring;
  }
  i = 0;
  cJSON_ArrayForEach(item, env_json) {
    if (!item->string)
      continue;
    env[i++] = cJSON_IsString(item) ? utilFormat("%s=%s", item->string, item->valuestring)
                                    : utilStrdup(item->string); // null: unset
  }

  name = stringToUtf8((unsigned char *)DEBUGGEE_BUFFER);
  id = name ? bufferFindByFilename(name) : -1;
  old = bufferFindById(id);
  layout = splitScreenGetLayout();
  for (i = 0; old && i < layout->num_panes; i++)
    if (layout->panes[i].buffer_id == id && i != layout->active_pane)
      pane = i; // the new program goes where the last one was
  if (old) {
    ptySessionClose(old); // a live one would make bufferClose() ask
    bufferClose(id);
  }
  ptySessionSetNextProgram(argv, cwd, (const char *const *)env);
  id = name ? bufferCreate(name, READ_AND_WRITE, TERMINAL_BUFF) : -1;
  ptySessionSetNextProgram(NULL, NULL, NULL);
  gfree(name);
  if (bufferFindById(id)) {
    pid = ptySessionPid(bufferFindById(id));
    if (showProgramPane(id, pane) != 0)
      E.prefix_state = 0; // bufferCreate() gave the terminal's keys, but
                          // the focus stayed on another buffer
  }

done:
  if (pid > 0) {
    body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "processId", (double)pid);
    respond(request, 1, body, NULL);
    editorSetStatusMessage("The program runs in %s: type to it, ESC for the editor's keys",
                           DEBUGGEE_BUFFER);
  } else {
    respond(request, 0, NULL, "couldn't start the program in a terminal");
  }
  for (i = 0; env && i < n_env; i++)
    gfree(env[i]);
  gfree(env);
  gfree((void *)argv);
}

// The adapter's own requests: runInTerminal; anything else is refused.
static void onRequest(cJSON *request) {
  const char *command = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "command"));
  if (command && !strcmp(command, "runInTerminal"))
    runInTerminal(request);
  else
    respond(request, 0, NULL, "not supported by beluga");
}

/***********************************************************************************************************************
 *
 *                                                 Events
 *
 **********************************************************************************************************************/

static void onStopped(cJSON *body) {
  cJSON *thread = cJSON_GetObjectItemCaseSensitive(body, "threadId");
  const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "reason"));
  const char *description =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "description"));
  cJSON *args;
  int *gen;
  if (cJSON_IsNumber(thread))
    s->thread_id = thread->valueint;
  // An "exception" says more in its description: lldb's crashes
  // ("EXC_BAD_ACCESS (code=1, address=0x0)"), and its pauses, which are a
  // "signal SIGSTOP".
  if (reason && !strcmp(reason, "exception") && description && *description)
    reason = description;
  // gdb stops the program it attaches to, and only reads its symbols then:
  // the breakpoints sent before came back pending ("No source file").
  // Send them again and let it go on, as it was before the attach.
  if (s->attach && reason && !strcmp(reason, "attach")) {
    sendAllBreakpoints();
    sendRequest("continue", threadArguments(), NULL, NULL);
    s->state = DAP_RUNNING;
    debugUiRefresh();
    return;
  }
  snprintf(s->stop_reason, sizeof(s->stop_reason), "%s", reason ? reason : "paused");
  s->state = DAP_STOPPED;
  outputFlushPartial();
  forgetStoppedState();
  gen = galloc(sizeof(int));
  if (gen)
    *gen = s->gen;
  args = threadArguments();
  cJSON_AddNumberToObject(args, "startFrame", 0);
  cJSON_AddNumberToObject(args, "levels", DAP_STACK_LEVELS);
  sendRequest("stackTrace", args, onStackTrace, gen);
  editorSetStatusMessage("Stopped: %s", s->stop_reason);
  debugUiRefresh();
}

static void onEvent(const char *event, cJSON *body) {
  cJSON *item;
  if (!strcmp(event, "initialized")) {
    // The adapter is ready for the breakpoints; configurationDone, once
    // they're all answered, lets the program run. (gdb answers it with an
    // error, "notStopped", and runs the program anyway.)
    s->configured = 1;
    s->config_waiting = sendAllBreakpoints() + sendExceptionBreakpoints();
    if (s->config_waiting == 0)
      configurationDone();
  } else if (!strcmp(event, "stopped")) {
    onStopped(body);
  } else if (!strcmp(event, "continued")) {
    s->state = DAP_RUNNING;
    forgetStoppedState();
  } else if (!strcmp(event, "output")) {
    item = cJSON_GetObjectItemCaseSensitive(body, "category");
    if (cJSON_GetStringValue(item) && !strcmp(cJSON_GetStringValue(item), "telemetry"))
      return;
    item = cJSON_GetObjectItemCaseSensitive(body, "output");
    if (cJSON_GetStringValue(item))
      outputAppend(cJSON_GetStringValue(item));
  } else if (!strcmp(event, "thread")) {
    item = cJSON_GetObjectItemCaseSensitive(body, "threadId");
    if (s->thread_id == 0 && cJSON_IsNumber(item))
      s->thread_id = item->valueint;
  } else if (!strcmp(event, "breakpoint")) {
    onBreakpointEvent(body);
  } else if (!strcmp(event, "exited")) {
    s->exit_code = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(body, "exitCode"));
  } else if (!strcmp(event, "terminated")) {
    // gdb 16 can send the program's last output after this, and drops it
    // if disconnect comes first: disconnect waits a moment (dapTick()).
    // Most adapters exit on their own instead; the session ends when the
    // adapter does (dapPoll()).
    if (!s->ending_at)
      s->ending_at = os_monotonic_ms() + TERMINATED_WAIT_MS;
    return;
  }
  debugUiRefresh();
}

static void handleMessage(const char *json) {
  cJSON *msg = cJSON_Parse(json);
  const char *type;
  const char *event;
  int seq;
  int i;
  DapHandler handler;
  void *ctx;
  if (!msg)
    return;
  type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "type"));
  if (type && !strcmp(type, "response")) {
    seq = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(msg, "request_seq"));
    for (i = 0; i < s->pending_count; i++) {
      if (s->pending[i].seq != seq)
        continue;
      handler = s->pending[i].handler;
      ctx = s->pending[i].ctx;
      s->pending[i] = s->pending[--s->pending_count];
      handler(msg, ctx);
      gfree(ctx);
      break;
    }
  } else if (type && !strcmp(type, "event")) {
    event = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "event"));
    onEvent(event ? event : "", cJSON_GetObjectItemCaseSensitive(msg, "body"));
  } else if (type && !strcmp(type, "request")) {
    onRequest(msg);
  }
  cJSON_Delete(msg);
}

static void onMessage(void *ctx, const char *body) {
  (void)ctx;
  appDebug("[DAP ←] %s\n", body);
  if (connected()) // not what followed the end of a session
    handleMessage(body);
}

void dapFeed(const char *data, const size_t len) {
  DapSession *saved = s;
  s = currentSession();
  jsonRpcStreamFeed(&s->recv, data, len, onMessage, NULL);
  s = saved;
}

static void pollSession(void) {
  char chunk[4096];
  long n = 0;
  int code;
  char message[160];
  if (!sessionStream(s))
    return;
  while (sessionStream(s) && (n = os_stream_read(sessionStream(s), chunk, sizeof(chunk), 0)) > 0)
    jsonRpcStreamFeed(&s->recv, chunk, (size_t)n, onMessage, NULL);
  if (sessionStream(s) && n < 0) {
    // The adapter's output has ended: it exited (or crashed), or closed
    // the connection.
    if (s->ending_at) {
      endTerminated();
      return;
    }
    code = -1;
    if (s->proc)
      os_proc_wait(s->proc, 100, &code);
    if (s->conn)
      snprintf(message, sizeof(message), "The debug adapter %s closed the connection", s->adapter);
    else if (code == 127)
      snprintf(message, sizeof(message), "Couldn't run the debug adapter %s: is it installed?",
               s->adapter);
    else
      snprintf(message, sizeof(message), "The debug adapter %s exited (code %d)", s->adapter, code);
    endSession(message, 1);
  }
}

void dapPoll(void) {
  DapSession *saved = s;
  int entered;
  int i;
  for (i = 0; i < DAP_MAX_SESSIONS; i++) {
    if (!sessionStream(&s_sessions[i]))
      continue;
    entered = enter(&s_sessions[i]);
    pollSession();
    leave(entered);
  }
  s = saved;
}

/***********************************************************************************************************************
 *
 *                                                 Commands
 *
 **********************************************************************************************************************/

static int requireStopped(void) {
  if (s->state == DAP_STOPPED)
    return 1;
  editorSetStatusMessage(s->state == DAP_OFF ? "No debug session (CTRL-x B c starts one)"
                                            : "The program is running (CTRL-x B p pauses it)");
  return 0;
}

static void resume(const char *command) {
  DapSession *saved = s;
  s = drivenSession();
  if (requireStopped()) {
    sendRequest(command, threadArguments(), NULL, NULL);
    s->state = DAP_RUNNING;
    forgetStoppedState();
    debugUiRefresh();
  }
  s = saved;
}

void dapContinue(void) { resume("continue"); }
void dapNext(void) { resume("next"); }
void dapStepIn(void) { resume("stepIn"); }
void dapStepOut(void) { resume("stepOut"); }

void dapPause(void) {
  DapSession *saved = s;
  s = drivenSession();
  if (s->state != DAP_RUNNING)
    editorSetStatusMessage(s->state == DAP_STOPPED ? "The program is already paused"
                                                  : "No debug session");
  else
    sendRequest("pause", threadArguments(), NULL, NULL);
  s = saved;
}

void dapEvaluate(const char *expression, const int repl) {
  DapSession *saved = s;
  cJSON *args;
  s = drivenSession();
  if (s->state == DAP_OFF) {
    editorSetStatusMessage("No debug session (CTRL-x B c starts one)");
  } else {
    args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "expression", expression);
    cJSON_AddStringToObject(args, "context", repl ? "repl" : "watch");
    if (s->state == DAP_STOPPED && s->selected < s->frame_count)
      cJSON_AddNumberToObject(args, "frameId", s->frames[s->selected].id);
    sendRequest("evaluate", args, onEvaluate, utilStrdup(expression));
  }
  s = saved;
}

/***********************************************************************************************************************
 *
 *                                                 Starting
 *
 **********************************************************************************************************************/

int dapRegisterAdapter(const char *ext, const char *const *argv, const char *launch_defaults) {
  DapAdapter *a;
  int i;
  for (i = 0; i < s_adapter_count; i++)
    if (!strcmp(s_adapters[i].ext, ext))
      return 0;
  if (s_adapter_count == DAP_MAX_ADAPTERS || !argv || !argv[0])
    return -1;
  a = &s_adapters[s_adapter_count++];
  memset(a, 0, sizeof(*a));
  snprintf(a->ext, sizeof(a->ext), "%s", ext);
  for (i = 0; i < DAP_MAX_ARGS && argv[i]; i++)
    a->argv[i] = utilStrdup(argv[i]);
  a->launch_defaults = launch_defaults ? utilStrdup(launch_defaults) : NULL;
  return 0;
}

// The adapter's own launch defaults, for what `launch` doesn't set.
static void addLaunchDefaults(cJSON *launch, const DapAdapter *adapter) {
  cJSON *defaults = adapter->launch_defaults ? cJSON_Parse(adapter->launch_defaults) : NULL;
  cJSON *item;
  cJSON_ArrayForEach(item, defaults) {
    if (item->string && !cJSON_GetObjectItemCaseSensitive(launch, item->string))
      cJSON_AddItemToObject(launch, item->string, cJSON_Duplicate(item, 1));
  }
  cJSON_Delete(defaults);
}

// What the panel calls the adapter: its program, or the one a launcher
// such as xcrun or env runs.
static const char *adapterName(const DapAdapter *adapter) {
  const char *name = adapter->argv[0];
  const char *slash = strrchr(name, '/');
  name = slash ? slash + 1 : name;
  if ((!strcmp(name, "xcrun") || !strcmp(name, "env")) && adapter->argv[1])
    return adapter->argv[1];
  return name;
}

/**
 * @brief The command line that starts `adapter`, in `argv` (room for
 * DAP_MAX_ARGS + 2). gdb gets the launch's program after its own
 * arguments, so it has the symbols before any breakpoint arrives; other
 * adapters run as registered.
 */
static void adapterArgv(const DapAdapter *adapter, const cJSON *launch, const char **argv) {
  const char *program = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(launch, "program"));
  const char *name = adapterName(adapter);
  int n;
  for (n = 0; n < DAP_MAX_ARGS && adapter->argv[n]; n++)
    argv[n] = adapter->argv[n];
  if (program && *program &&
      (!strcmp(name, "gdb") || !strcmp(name, "gdb.exe") || !strcmp(name, "gdb-multiarch")))
    argv[n++] = program;
  argv[n] = NULL;
}

/**
 * @brief The adapter for `ext`, or for the active buffer's extension, or
 * the first one registered.
 */
static const DapAdapter *findAdapter(const char *ext) {
  struct buffer_t *buf;
  char *name;
  const char *dot;
  char buf_ext[16] = "";
  int i;
  if (!ext || !*ext) {
    buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
    name = (buf && buf->type == FILE_BUFF && buf->filename) ? (char *)utf8ToString(buf->filename)
                                                            : NULL;
    dot = name ? strrchr(name, '.') : NULL;
    if (dot)
      snprintf(buf_ext, sizeof(buf_ext), "%s", dot + 1);
    gfree(name);
    ext = buf_ext;
  }
  for (i = 0; i < s_adapter_count; i++)
    if (!strcmp(s_adapters[i].ext, ext))
      return &s_adapters[i];
  return s_adapter_count > 0 ? &s_adapters[0] : NULL;
}

static void sendInitialize(void) {
  cJSON *args = cJSON_CreateObject();
  cJSON_AddStringToObject(args, "clientID", "beluga");
  cJSON_AddStringToObject(args, "clientName", "beluga");
  cJSON_AddStringToObject(args, "adapterID", s->adapter_id);
  cJSON_AddStringToObject(args, "pathFormat", "path");
  cJSON_AddBoolToObject(args, "linesStartAt1", 1);
  cJSON_AddBoolToObject(args, "columnsStartAt1", 1);
  cJSON_AddBoolToObject(args, "supportsVariableType", 1);
  cJSON_AddBoolToObject(args, "supportsRunInTerminalRequest", 1);
  sendRequest("initialize", args, onInitialize, NULL);
}

/**
 * @brief "tcp:HOST:PORT" (an adapter already listening there) into
 * s->tcp_host and s->tcp_port. @return 1 if `spec` is one.
 */
static int parseTcpAddress(const char *spec) {
  const char *colon;
  int port;
  if (strncmp(spec, "tcp:", 4) != 0)
    return 0;
  colon = strrchr(spec + 4, ':');
  if (!colon || colon == spec + 4 || (size_t)(colon - spec - 4) >= sizeof(s->tcp_host))
    return 0;
  port = atoi(colon + 1);
  if (port <= 0 || port > 65535)
    return 0;
  memcpy(s->tcp_host, spec + 4, (size_t)(colon - spec - 4));
  s->tcp_host[colon - spec - 4] = '\0';
  s->tcp_port = port;
  return 1;
}

/**
 * @brief Replaces "{port}" in the arguments of `argv` by a free local port,
 * for an adapter told where to listen (dlv dap --listen=127.0.0.1:{port}).
 * The new strings go in `storage`. Sets s->tcp_host and s->tcp_port when
 * there was one. @return 0, or -1 when no port could be found.
 */
static int substitutePort(const char **argv, char storage[][256]) {
  const char *mark;
  int port = 0;
  int i;
  for (i = 1; argv[i]; i++) {
    mark = strstr(argv[i], "{port}");
    if (!mark)
      continue;
    if (port == 0 && (port = os_tcp_free_port()) <= 0)
      return -1;
    snprintf(storage[i], 256, "%.*s%d%s", (int)(mark - argv[i]), argv[i], port, mark + 6);
    argv[i] = storage[i];
  }
  if (port > 0) {
    snprintf(s->tcp_host, sizeof(s->tcp_host), "127.0.0.1");
    s->tcp_port = port;
  }
  return 0;
}

static int startSession(const char *ext, const char *launch_json, int attach);

int dapStart(const char *ext, const char *launch_json) {
  DapSession *saved = s;
  int rc;
  s = currentSession();
  rc = startSession(ext, launch_json, 0);
  s = saved;
  return rc;
}

int dapAttach(const char *ext, const char *attach_json) {
  DapSession *saved = s;
  int rc;
  s = currentSession();
  rc = startSession(ext, attach_json, 1);
  s = saved;
  return rc;
}

static int startSession(const char *ext, const char *launch_json, const int attach) {
  const DapAdapter *adapter;
  cJSON *launch;
  OsSpawn spawn;
  const char *argv[DAP_MAX_ARGS + 2];
  char port_args[DAP_MAX_ARGS + 2][256];
  char *root;
  const char *debug_log;
  Breakpoint *all;
  int count;
  int i;
  if (s->state != DAP_OFF) {
    editorSetStatusMessage("This project's debug session is already running (CTRL-x B q stops it)");
    return -1;
  }
  adapter = findAdapter(ext);
  if (!adapter) {
    editorSetStatusMessage("No debug adapter: add one with (dap-adapter \"ext\" \"program\" ...)");
    return -1;
  }
  launch = cJSON_Parse(launch_json ? launch_json : "{}");
  if (!cJSON_IsObject(launch)) {
    cJSON_Delete(launch);
    editorSetStatusMessage("debug-start: the launch arguments aren't a JSON object");
    return -1;
  }
  root = projectRootOrCwd();
  if (root && !attach && !cJSON_GetObjectItemCaseSensitive(launch, "cwd"))
    cJSON_AddStringToObject(launch, "cwd", root);
  if (!attach)
    addLaunchDefaults(launch, adapter);

  // stdin/stdout carry the protocol; the adapter's stderr goes where the
  // language servers' does (discarded, or the debug log). One that
  // listens on a port is connected to instead (dapTick()), and what it
  // prints goes to the output.
  s->tcp_port = 0;
  if (!parseTcpAddress(adapter->argv[0])) {
    memset(&spawn, 0, sizeof(spawn));
    adapterArgv(adapter, launch, argv);
    if (substitutePort(argv, port_args) != 0) {
      gfree(root);
      cJSON_Delete(launch);
      editorSetStatusMessage("No free port for %s to listen on", adapter->argv[0]);
      return -1;
    }
    spawn.argv = argv;
    spawn.cwd = root;
    spawn.stdin_pipe = !s->tcp_port;
    debug_log = appDebugLogPath();
    spawn.stderr_mode = s->tcp_port ? OS_STDERR_MERGE
                        : debug_log ? OS_STDERR_FILE
                                    : OS_STDERR_DISCARD;
    spawn.stderr_file = debug_log;
    spawn.no_prompt = 1;
    s->proc = os_proc_spawn(&spawn);
    if (!s->proc) {
      s->tcp_port = 0;
      gfree(root);
      cJSON_Delete(launch);
      editorSetStatusMessage("Couldn't start %s", adapter->argv[0]);
      return -1;
    }
  }

  jsonRpcStreamFree(&s->recv);
  s->seq = 0;
  s->gen++;
  s->thread_id = 0;
  s->exit_code = -1;
  s->configured = 0;
  s->config_waiting = 0;
  s->config_early = 0;
  s->ending_at = 0;
  s->disconnected = 0;
  s->stop_reason[0] = '\0';
  s->launch = launch;
  s->attach = attach;
  s->root = root;
  snprintf(s->last_root, sizeof(s->last_root), "%s", root ? root : "");
  s->owner = s_window_current ? s_window_current() : 0;
  s->launch_at = 0;
  s->state = DAP_STARTING;
  snprintf(s->adapter, sizeof(s->adapter), "%s", adapterName(adapter));
  outputClear();
  all = breakpointsAll(&count);
  for (i = 0; i < count; i++)
    all[i].verified = 0;
  breakpointsSetListener(onBreakpointsChanged);

  snprintf(s->adapter_id, sizeof(s->adapter_id), "%s", adapter->ext);
  if (s->tcp_port)
    s->connect_until = os_monotonic_ms() + DAP_CONNECT_TIMEOUT_MS; // dapTick() connects
  else
    sendInitialize();

  debugUiOpenPanel();
  editorSetStatusMessage("Starting %s…", s->adapter);
  return 0;
}

void dapFree(void) {
  int i;
  int j;
  for (i = 0; i < DAP_MAX_SESSIONS; i++) {
    s = &s_sessions[i];
    // On the way out: no hook, the Lisp side is being torn down.
    if (s->state != DAP_OFF)
      stopSession(0);
    outputClear();
    jsonRpcStreamFree(&s->recv);
    s->last_root[0] = '\0';
  }
  s = &s_sessions[0];
  s_window_current = NULL;
  s_window_enter = NULL;
  s_window_leave = NULL;
  breakpointsSetListener(NULL);
  for (i = 0; i < s_adapter_count; i++) {
    for (j = 0; j < DAP_MAX_ARGS && s_adapters[i].argv[j]; j++)
      gfree(s_adapters[i].argv[j]);
    gfree(s_adapters[i].launch_defaults);
  }
  s_adapter_count = 0;
  gfree(s_exception_choice);
  s_exception_choice = NULL;
  for (i = 0; i < s_watch_count; i++)
    gfree(s_watches[i]);
  gfree(s_watches);
  s_watches = NULL;
  s_watch_count = 0;
}
