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
 * if it comes, launch follows configurationDone; if not, it goes first. A "stopped" event asks for
 * the call stack, the selected frame's scopes and their variables, and
 * each answer refreshes the panel (debug_ui.c). Variables deeper down are
 * fetched when the panel expands them.
 *
 * Answers can arrive after the state they were asked about is gone (the
 * program was continued, another frame selected): every request that
 * fills the stack or the variables carries the generation it was sent
 * in, and s.gen moves on whenever that state is thrown away, so a late
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

static struct
{
    OsProc *proc;
    JsonRpcStream recv;
    int seq;
    DapState state;
    char adapter[64];
    cJSON *launch;         // launch arguments, until launch is sent
    long long launch_at;   // when launch goes anyway (ms), 0 before initialize
    int configured;        // breakpoints sent: changes go out as they happen
    // setBreakpoints still unanswered before configurationDone may go:
    // gdb lets the program run without them otherwise. -1 once it's sent.
    int config_waiting;
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
} s = {.exit_code = -1};

// Output, oldest line first; a line still being written waits in partial.
static char *s_output[DAP_OUTPUT_LINES];
static int s_output_count;
static char *s_partial;

/***********************************************************************************************************************
 *
 *                                                 Output
 *
 **********************************************************************************************************************/

static void outputPushLine(const char *text, const size_t len) {
  char *line = utilStrndup(text, len);
  if (!line)
    return;
  if (s_output_count == DAP_OUTPUT_LINES) {
    gfree(s_output[0]);
    memmove(s_output, s_output + 1, sizeof(char *) * (DAP_OUTPUT_LINES - 1));
    s_output_count--;
  }
  s_output[s_output_count++] = line;
}

static void outputFlushPartial(void) {
  if (s_partial && *s_partial)
    outputPushLine(s_partial, strlen(s_partial));
  gfree(s_partial);
  s_partial = NULL;
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
  joined = utilFormat("%s%s", s_partial ? s_partial : "", text);
  gfree(s_partial);
  s_partial = NULL;
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
    s_partial = utilStrdup(start);
  gfree(joined);
}

static void outputClear(void) {
  int i;
  for (i = 0; i < s_output_count; i++)
    gfree(s_output[i]);
  s_output_count = 0;
  gfree(s_partial);
  s_partial = NULL;
}

const char *const *dapOutputTail(int *count) {
  *count = s_output_count;
  return (const char *const *)s_output;
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
  freeVars(s.scopes, s.scope_count);
  s.scopes = NULL;
  s.scope_count = 0;
}

static void clearFrames(void) {
  int i;
  for (i = 0; i < s.frame_count; i++) {
    gfree(s.frames[i].name);
    gfree(s.frames[i].path);
  }
  gfree(s.frames);
  s.frames = NULL;
  s.frame_count = 0;
  s.selected = 0;
}

// The stack and variables no longer describe the program: drop them, and
// any answer still on its way about them.
static void forgetStoppedState(void) {
  s.gen++;
  clearScopes();
  clearFrames();
}

const DapFrame *dapFrames(int *count, int *selected) {
  *count = s.frame_count;
  *selected = s.selected;
  return s.frames;
}

DapVar *dapScopes(int *count) {
  *count = s.scope_count;
  return s.scopes;
}

int dapCurrentLine(const char **path, int *line) {
  if (s.state != DAP_STOPPED || s.selected >= s.frame_count || !s.frames[s.selected].path)
    return 0;
  *path = s.frames[s.selected].path;
  *line = s.frames[s.selected].line;
  return 1;
}

DapState dapState(void) { return s.state; }
const char *dapStopReason(void) { return s.stop_reason; }
const char *dapAdapterName(void) { return s.adapter; }
int dapExitCode(void) { return s.exit_code; }
OsStream *dapStream(void) { return s.proc ? os_proc_output(s.proc) : NULL; }

/***********************************************************************************************************************
 *
 *                                                 Transport
 *
 **********************************************************************************************************************/

static void clearPending(void) {
  int i;
  for (i = 0; i < s.pending_count; i++)
    gfree(s.pending[i].ctx);
  s.pending_count = 0;
}

/**
 * @brief Sends request `command` with `arguments` (taken over; NULL for
 * none). `handler` gets the response, then `ctx` is freed.
 * @return the request's seq, or -1 if there's no session.
 */
static int sendRequest(const char *command, cJSON *arguments, const DapHandler handler,
                       void *ctx) {
  cJSON *msg;
  char *text;
  int seq;
  if (!s.proc) {
    cJSON_Delete(arguments);
    gfree(ctx);
    return -1;
  }
  seq = ++s.seq;
  msg = cJSON_CreateObject();
  cJSON_AddNumberToObject(msg, "seq", seq);
  cJSON_AddStringToObject(msg, "type", "request");
  cJSON_AddStringToObject(msg, "command", command);
  if (arguments)
    cJSON_AddItemToObject(msg, "arguments", arguments);
  text = cJSON_PrintUnformatted(msg);
  cJSON_Delete(msg);
  if (text) {
    jsonRpcWrite(s.proc, text, "DAP");
    gfree(text);
  }
  if (handler && s.pending_count < DAP_MAX_PENDING) {
    s.pending[s.pending_count].seq = seq;
    s.pending[s.pending_count].handler = handler;
    s.pending[s.pending_count].ctx = ctx;
    s.pending_count++;
  } else {
    gfree(ctx);
  }
  return seq;
}

static cJSON *threadArguments(void) {
  cJSON *args = cJSON_CreateObject();
  cJSON_AddNumberToObject(args, "threadId", s.thread_id);
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
  const int had_session = s.state != DAP_OFF;
  outputFlushPartial();
  forgetStoppedState();
  clearPending();
  // s.recv stays: this may run from inside dapFeed(), which is still
  // reading it. It's emptied when the next session starts.
  cJSON_Delete(s.launch);
  s.launch = NULL;
  if (s.proc) {
    os_proc_free(s.proc);
    s.proc = NULL;
  }
  s.state = DAP_OFF;
  s.configured = 0;
  if (message)
    editorSetStatusMessage("%s", message);
  debugUiRefresh();
  if (had_session && run_hook && E.initialized)
    runHooks("debug-exited", lisp_cons(lisp_make_int(s.exit_code), lisp_null(), E.ctx));
}

/**
 * @brief Asks the adapter to end the program and itself, then ends the
 * session.
 */
static void stopSession(const int run_hook) {
  cJSON *args;
  if (s.state == DAP_OFF)
    return;
  args = cJSON_CreateObject();
  cJSON_AddBoolToObject(args, "terminateDebuggee", 1);
  sendRequest("disconnect", args, NULL, NULL);
  // Give it a moment to end the program and exit on its own;
  // os_proc_free() kills whatever is left.
  if (s.proc)
    os_proc_wait(s.proc, 1000, NULL);
  endSession("Debug session ended", run_hook);
}

void dapStop(void) { stopSession(1); }

/***********************************************************************************************************************
 *
 *                                                 Breakpoints
 *
 **********************************************************************************************************************/

static void onLaunch(cJSON *response, void *ctx);

static void sendLaunch(void) {
  cJSON *launch = s.launch;
  s.launch = NULL;
  if (launch)
    sendRequest("launch", launch, onLaunch, NULL);
}

// The program may run once the adapter has every breakpoint.
static void configurationDone(void) {
  s.config_waiting = -1;
  sendRequest("configurationDone", NULL, NULL, NULL);
  sendLaunch(); // if it waited for this
  if (s.state == DAP_STARTING)
    s.state = DAP_RUNNING;
}

static void onSetBreakpoints(cJSON *response, void *ctx) {
  const char *path = ctx;
  cJSON *answers = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(response, "body"), "breakpoints");
  cJSON *answer;
  Breakpoint *all;
  int count;
  int i;
  if (s.config_waiting > 0 && --s.config_waiting == 0)
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
    if (i == 0 || strcmp(all[i].path, all[i - 1].path) != 0) {
      sendBreakpoints(all[i].path);
      sent++;
    }
  return sent;
}

// breakpoints.c's listener: a file's breakpoints changed.
static void onBreakpointsChanged(const char *path) {
  if (s.proc && s.configured)
    sendBreakpoints(path);
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
  if (req->gen != s.gen)
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
  req->gen = s.gen;
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
  // The panel's row may be older than the tree; only a node still in it.
  if (!var || !varInTree(var, s.scopes, s.scope_count) || var->ref <= 0)
    return;
  var->expanded = !var->expanded;
  if (var->expanded && !var->loaded)
    requestVariables(var);
  debugUiRefresh();
}

static void onScopes(cJSON *response, void *ctx) {
  const int gen = *(const int *)ctx;
  cJSON *list;
  cJSON *item;
  const char *hint;
  int n;
  int i;
  if (gen != s.gen || !responseOk(response))
    return;
  list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "body"),
                                          "scopes");
  n = cJSON_GetArraySize(list);
  clearScopes();
  s.scopes = n > 0 ? gcalloc((size_t)n, sizeof(DapVar)) : NULL;
  if (!s.scopes) {
    debugUiRefresh();
    return;
  }
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (i >= n)
      break;
    s.scopes[i].name = jsonString(item, "name");
    s.scopes[i].ref = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItemCaseSensitive(item, "variablesReference"));
    i++;
  }
  s.scope_count = i;
  // Open the cheap ones (locals, arguments); "expensive" ones (globals)
  // and the registers wait to be opened from the panel.
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (i >= s.scope_count)
      break;
    hint = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "presentationHint"));
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "expensive")) &&
        !(hint && !strcmp(hint, "registers")) && s.scopes[i].ref > 0) {
      s.scopes[i].expanded = 1;
      requestVariables(&s.scopes[i]);
    }
    i++;
  }
  debugUiRefresh();
}

void dapSelectFrame(const int index) {
  cJSON *args;
  int *gen;
  if (s.state != DAP_STOPPED || index < 0 || index >= s.frame_count)
    return;
  s.gen++;
  clearScopes();
  s.selected = index;
  if (s.frames[index].path)
    debugUiShowLocation(s.frames[index].path, s.frames[index].line);
  gen = galloc(sizeof(int));
  if (gen)
    *gen = s.gen;
  args = cJSON_CreateObject();
  cJSON_AddNumberToObject(args, "frameId", s.frames[index].id);
  sendRequest("scopes", args, onScopes, gen);
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
  if (gen != s.gen)
    return;
  if (!responseOk(response)) {
    editorSetStatusMessage("Call stack: %s", responseMessage(response));
    return;
  }
  list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "body"),
                                          "stackFrames");
  n = cJSON_GetArraySize(list);
  clearFrames();
  s.frames = n > 0 ? gcalloc((size_t)n, sizeof(DapFrame)) : NULL;
  first_with_source = -1;
  i = 0;
  cJSON_ArrayForEach(item, list) {
    if (!s.frames || i >= n)
      break;
    s.frames[i].id = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
    s.frames[i].name = jsonString(item, "name");
    source = cJSON_GetObjectItemCaseSensitive(item, "source");
    // A source with a sourceReference only exists inside the adapter
    // (lldb-dap's disassembly, "/usr/lib/dyld`start"): no file to open.
    if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(source, "sourceReference")) > 0)
      s.frames[i].path = NULL;
    else
      s.frames[i].path = jsonString(source, "path");
    s.frames[i].line =
        (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "line")) - 1;
    if (s.frames[i].line < 0)
      s.frames[i].line = 0;
    if (first_with_source < 0 && s.frames[i].path)
      first_with_source = i;
    i++;
  }
  s.frame_count = i;
  // Stopped inside code without debug info (a library): show the first
  // frame that has a source file.
  dapSelectFrame(first_with_source >= 0 ? first_with_source : 0);
  debugUiRefresh();

  if (E.initialized) {
    path = s.frame_count > 0 ? s.frames[s.selected].path : NULL;
    runHooks("debug-stopped",
             lisp_cons(path ? lisp_make_string2(path, E.ctx) : lisp_false(),
                       lisp_cons(lisp_make_int(s.frame_count > 0 ? s.frames[s.selected].line + 1 : 0),
                                 lisp_cons(lisp_make_string2(s.stop_reason, E.ctx), lisp_null(), E.ctx),
                                 E.ctx),
                       E.ctx));
  }
}

static void onLaunch(cJSON *response, void *ctx) {
  (void)ctx;
  if (!responseOk(response)) {
    outputAppend("Launch failed: ");
    outputAppend(responseMessage(response));
    outputAppend("\n");
    endSession(NULL, 1);
    editorSetStatusMessage("Couldn't launch the program: %s", responseMessage(response));
  }
}

#define LAUNCH_WAIT_MS 300 // for an "initialized" event before launch

static void onInitialize(cJSON *response, void *ctx) {
  (void)ctx;
  if (!responseOk(response)) {
    endSession(NULL, 1);
    editorSetStatusMessage("The debug adapter refused to start: %s", responseMessage(response));
    return;
  }
  s.launch_at = os_monotonic_ms() + LAUNCH_WAIT_MS;
}

void dapTick(void) {
  if (s.launch && s.launch_at && os_monotonic_ms() >= s.launch_at)
    sendLaunch();
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

// Answers the adapter's own requests (runInTerminal, ...): not supported.
static void refuseRequest(cJSON *request) {
  cJSON *msg;
  char *text;
  cJSON *seq = cJSON_GetObjectItemCaseSensitive(request, "seq");
  const char *command = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "command"));
  msg = cJSON_CreateObject();
  cJSON_AddNumberToObject(msg, "seq", ++s.seq);
  cJSON_AddStringToObject(msg, "type", "response");
  cJSON_AddNumberToObject(msg, "request_seq", cJSON_GetNumberValue(seq));
  cJSON_AddStringToObject(msg, "command", command ? command : "");
  cJSON_AddBoolToObject(msg, "success", 0);
  cJSON_AddStringToObject(msg, "message", "not supported by beluga");
  text = cJSON_PrintUnformatted(msg);
  cJSON_Delete(msg);
  if (text) {
    jsonRpcWrite(s.proc, text, "DAP");
    gfree(text);
  }
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
    s.thread_id = thread->valueint;
  // An "exception" says more in its description: lldb's crashes
  // ("EXC_BAD_ACCESS (code=1, address=0x0)"), and its pauses, which are a
  // "signal SIGSTOP".
  if (reason && !strcmp(reason, "exception") && description && *description)
    reason = description;
  snprintf(s.stop_reason, sizeof(s.stop_reason), "%s", reason ? reason : "paused");
  s.state = DAP_STOPPED;
  outputFlushPartial();
  forgetStoppedState();
  gen = galloc(sizeof(int));
  if (gen)
    *gen = s.gen;
  args = threadArguments();
  cJSON_AddNumberToObject(args, "startFrame", 0);
  cJSON_AddNumberToObject(args, "levels", DAP_STACK_LEVELS);
  sendRequest("stackTrace", args, onStackTrace, gen);
  editorSetStatusMessage("Stopped: %s", s.stop_reason);
  debugUiRefresh();
}

static void onEvent(const char *event, cJSON *body) {
  cJSON *item;
  char message[64];
  if (!strcmp(event, "initialized")) {
    // The adapter is ready for the breakpoints; configurationDone, once
    // they're all answered, lets the program run. (gdb answers it with an
    // error, "notStopped", and runs the program anyway.)
    s.configured = 1;
    s.config_waiting = sendAllBreakpoints();
    if (s.config_waiting == 0)
      configurationDone();
  } else if (!strcmp(event, "stopped")) {
    onStopped(body);
  } else if (!strcmp(event, "continued")) {
    s.state = DAP_RUNNING;
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
    if (s.thread_id == 0 && cJSON_IsNumber(item))
      s.thread_id = item->valueint;
  } else if (!strcmp(event, "breakpoint")) {
    onBreakpointEvent(body);
  } else if (!strcmp(event, "exited")) {
    s.exit_code = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(body, "exitCode"));
  } else if (!strcmp(event, "terminated")) {
    snprintf(message, sizeof(message), "The program exited with code %d", s.exit_code);
    // Polite, though most adapters exit on their own at this point.
    sendRequest("disconnect", NULL, NULL, NULL);
    if (s.proc)
      os_proc_wait(s.proc, 200, NULL);
    endSession(message, 1);
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
    for (i = 0; i < s.pending_count; i++) {
      if (s.pending[i].seq != seq)
        continue;
      handler = s.pending[i].handler;
      ctx = s.pending[i].ctx;
      s.pending[i] = s.pending[--s.pending_count];
      handler(msg, ctx);
      gfree(ctx);
      break;
    }
  } else if (type && !strcmp(type, "event")) {
    event = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "event"));
    onEvent(event ? event : "", cJSON_GetObjectItemCaseSensitive(msg, "body"));
  } else if (type && !strcmp(type, "request")) {
    refuseRequest(msg);
  }
  cJSON_Delete(msg);
}

static void onMessage(void *ctx, const char *body) {
  (void)ctx;
  appDebug("[DAP ←] %s\n", body);
  if (s.proc) // what followed the end of a session
    handleMessage(body);
}

void dapFeed(const char *data, const size_t len) {
  jsonRpcStreamFeed(&s.recv, data, len, onMessage, NULL);
}

void dapPoll(void) {
  char chunk[4096];
  long n;
  int code;
  char message[160];
  if (!s.proc)
    return;
  while (s.proc && (n = os_stream_read(os_proc_output(s.proc), chunk, sizeof(chunk), 0)) > 0)
    dapFeed(chunk, (size_t)n);
  if (s.proc && n < 0) {
    // The adapter's output has ended: it exited (or crashed).
    code = -1;
    os_proc_wait(s.proc, 100, &code);
    if (code == 127)
      snprintf(message, sizeof(message), "Couldn't run the debug adapter %s: is it installed?",
               s.adapter);
    else
      snprintf(message, sizeof(message), "The debug adapter %s exited (code %d)", s.adapter, code);
    endSession(message, 1);
  }
}

/***********************************************************************************************************************
 *
 *                                                 Commands
 *
 **********************************************************************************************************************/

static int requireStopped(void) {
  if (s.state == DAP_STOPPED)
    return 1;
  editorSetStatusMessage(s.state == DAP_OFF ? "No debug session (CTRL-x B c starts one)"
                                            : "The program is running (CTRL-x B p pauses it)");
  return 0;
}

static void resume(const char *command) {
  if (!requireStopped())
    return;
  sendRequest(command, threadArguments(), NULL, NULL);
  s.state = DAP_RUNNING;
  forgetStoppedState();
  debugUiRefresh();
}

void dapContinue(void) { resume("continue"); }
void dapNext(void) { resume("next"); }
void dapStepIn(void) { resume("stepIn"); }
void dapStepOut(void) { resume("stepOut"); }

void dapPause(void) {
  if (s.state != DAP_RUNNING) {
    editorSetStatusMessage(s.state == DAP_STOPPED ? "The program is already paused"
                                                  : "No debug session");
    return;
  }
  sendRequest("pause", threadArguments(), NULL, NULL);
}

void dapEvaluate(const char *expression, const int repl) {
  cJSON *args;
  if (s.state == DAP_OFF) {
    editorSetStatusMessage("No debug session (CTRL-x B c starts one)");
    return;
  }
  args = cJSON_CreateObject();
  cJSON_AddStringToObject(args, "expression", expression);
  cJSON_AddStringToObject(args, "context", repl ? "repl" : "watch");
  if (s.state == DAP_STOPPED && s.selected < s.frame_count)
    cJSON_AddNumberToObject(args, "frameId", s.frames[s.selected].id);
  sendRequest("evaluate", args, onEvaluate, utilStrdup(expression));
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

int dapStart(const char *ext, const char *launch_json) {
  const DapAdapter *adapter;
  cJSON *launch;
  cJSON *args;
  OsSpawn spawn;
  char *root;
  const char *debug_log;
  Breakpoint *all;
  int count;
  int i;
  if (s.state != DAP_OFF) {
    editorSetStatusMessage("A debug session is already running (CTRL-x B q stops it)");
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
  if (root && !cJSON_GetObjectItemCaseSensitive(launch, "cwd"))
    cJSON_AddStringToObject(launch, "cwd", root);
  addLaunchDefaults(launch, adapter);

  // stdin/stdout carry the protocol; the adapter's stderr goes where the
  // language servers' does (discarded, or the debug log).
  memset(&spawn, 0, sizeof(spawn));
  spawn.argv = (const char *const *)adapter->argv;
  spawn.cwd = root;
  spawn.stdin_pipe = 1;
  debug_log = appDebugLogPath();
  spawn.stderr_mode = debug_log ? OS_STDERR_FILE : OS_STDERR_DISCARD;
  spawn.stderr_file = debug_log;
  spawn.no_prompt = 1;
  s.proc = os_proc_spawn(&spawn);
  gfree(root);
  if (!s.proc) {
    cJSON_Delete(launch);
    editorSetStatusMessage("Couldn't start %s", adapter->argv[0]);
    return -1;
  }

  jsonRpcStreamFree(&s.recv);
  s.seq = 0;
  s.gen++;
  s.thread_id = 0;
  s.exit_code = -1;
  s.configured = 0;
  s.config_waiting = 0;
  s.stop_reason[0] = '\0';
  s.launch = launch;
  s.launch_at = 0;
  s.state = DAP_STARTING;
  snprintf(s.adapter, sizeof(s.adapter), "%s", adapterName(adapter));
  outputClear();
  all = breakpointsAll(&count);
  for (i = 0; i < count; i++)
    all[i].verified = 0;
  breakpointsSetListener(onBreakpointsChanged);

  args = cJSON_CreateObject();
  cJSON_AddStringToObject(args, "clientID", "beluga");
  cJSON_AddStringToObject(args, "clientName", "beluga");
  cJSON_AddStringToObject(args, "adapterID", adapter->ext);
  cJSON_AddStringToObject(args, "pathFormat", "path");
  cJSON_AddBoolToObject(args, "linesStartAt1", 1);
  cJSON_AddBoolToObject(args, "columnsStartAt1", 1);
  cJSON_AddBoolToObject(args, "supportsVariableType", 1);
  cJSON_AddBoolToObject(args, "supportsRunInTerminalRequest", 0);
  sendRequest("initialize", args, onInitialize, NULL);

  debugUiOpenPanel();
  editorSetStatusMessage("Starting %s…", s.adapter);
  return 0;
}

void dapFree(void) {
  int i;
  int j;
  // On the way out: no hook, the Lisp side is being torn down.
  if (s.state != DAP_OFF)
    stopSession(0);
  outputClear();
  jsonRpcStreamFree(&s.recv);
  breakpointsSetListener(NULL);
  for (i = 0; i < s_adapter_count; i++) {
    for (j = 0; j < DAP_MAX_ARGS && s_adapters[i].argv[j]; j++)
      gfree(s_adapters[i].argv[j]);
    gfree(s_adapters[i].launch_defaults);
  }
  s_adapter_count = 0;
}
