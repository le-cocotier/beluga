/**
 * @file builtins_debug.c
 * @brief The debugger's Lisp builtins: breakpoints, adapters, the session
 */

#include "../include/builtins.h"

#include "../include/breakpoints.h"
#include "../include/cJSON.h"
#include "../include/dap.h"
#include "../include/debug_ui.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/input.h"
#include "../include/list_popup.h"
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stddef.h>
#include <string.h>

extern EditorConfig E;

// What the last (debug-start) prompt was answered with, offered again.
static char *s_last_program;
static char *s_last_args;

/**
 * @brief The active buffer's file and the cursor's row, or 0 (with a
 * status message) when the active buffer isn't a file.
 */
static int cursorLocation(char **path, int *line) {
  struct buffer_t *buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
  *path = breakpointsBufferPath(buf);
  if (!*path) {
    editorSetStatusMessage("Breakpoints go in a file buffer");
    return 0;
  }
  *line = buf->y;
  return 1;
}

/**
 * @brief Lisp function: toggle a breakpoint
 * @details (debug-toggle-breakpoint) at the cursor's line, or
 * (debug-toggle-breakpoint "path" line) with a 1-based line.
 * @return #t if a breakpoint was added, #f if one was removed
 */
Lisp l_debugToggleBreakpoint(Lisp args, LispError *e, LispContext ctx) {
  char *path;
  int line;
  int added;
  (void)ctx;
  if (lisp_is_pair(args)) {
    if (lisp_type(lisp_car(args)) != LISP_STRING || !lisp_is_pair(lisp_cdr(args))) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    path = utilStrdup(lisp_string(lisp_car(args)));
    line = (int)lisp_int(lisp_car(lisp_cdr(args))) - 1;
  } else if (!cursorLocation(&path, &line)) {
    return lisp_false();
  }
  added = breakpointToggle(path, line);
  if (added >= 0)
    editorSetStatusMessage("%s breakpoint at line %d", added ? "Added" : "Removed", line + 1);
  gfree(path);
  return lisp_make_bool(added == 1);
}

// One of the texts a breakpoint can carry, set by a command.
typedef struct
{
    size_t field; // in Breakpoint
    int (*set)(const char *path, int line, const char *text);
    const char *prompt;
    const char *set_message;   // with the line and the text
    const char *clear_message; // with the line
} BreakpointText;

/**
 * @brief Sets the text `what` describes on the breakpoint at the cursor:
 * the argument, or what the user types (the current one offered).
 */
static Lisp setBreakpointText(Lisp args, LispError *e, const BreakpointText *what) {
  char *path;
  int line;
  Utf8Char *input;
  char *text;
  const Breakpoint *bp;
  const char *current;
  Utf8Char *placeholder;
  if (!cursorLocation(&path, &line))
    return lisp_null();
  if (lisp_is_pair(args)) {
    if (lisp_type(lisp_car(args)) != LISP_STRING) {
      gfree(path);
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    text = utilStrdup(lisp_string(lisp_car(args)));
  } else {
    bp = breakpointAt(path, line);
    current = bp ? *(char *const *)((const char *)bp + what->field) : NULL;
    placeholder = current ? stringToUtf8((unsigned char *)current) : NULL;
    input = editorPrompt(what->prompt, placeholder, 0);
    gfree(placeholder);
    if (!input) {
      gfree(path);
      return lisp_null();
    }
    text = (char *)utf8ToString(input);
    gfree(input);
  }
  if (what->set(path, line, text) == 0) {
    if (text && *text)
      editorSetStatusMessage(what->set_message, line + 1, text);
    else
      editorSetStatusMessage(what->clear_message, line + 1);
  }
  gfree(text);
  gfree(path);
  return lisp_null();
}

/**
 * @brief Lisp function: give the breakpoint at the cursor a condition
 * @details (debug-breakpoint-condition ["expression"]) — asks for the
 * expression when it isn't given. The program only stops there when the
 * expression is true (in the language being debugged: "i == 3"). An empty
 * one makes it an ordinary breakpoint again. Adds the breakpoint if the
 * line has none.
 * @return lisp_null()
 */
Lisp l_debugBreakpointCondition(Lisp args, LispError *e, LispContext ctx) {
  static const BreakpointText what = {
      offsetof(Breakpoint, condition), breakpointSetCondition,
      "Break when: %u (ESC to cancel)", "Breakpoint at line %d stops when %s",
      "Breakpoint at line %d always stops"};
  (void)ctx;
  return setBreakpointText(args, e, &what);
}

/**
 * @brief Lisp function: how many hits the breakpoint at the cursor waits for
 * @details (debug-breakpoint-hit-count ["count"]) — asks when it isn't
 * given. The adapter decides what it understands: a number ("3": from the
 * third hit on) everywhere, gdb also "> 5" or "% 2" (every other one). An
 * empty one stops every time again. Adds the breakpoint if the line has
 * none.
 * @return lisp_null()
 */
Lisp l_debugBreakpointHitCount(Lisp args, LispError *e, LispContext ctx) {
  static const BreakpointText what = {
      offsetof(Breakpoint, hit_condition), breakpointSetHitCondition,
      "Stop after how many hits: %u (ESC to cancel)", "Breakpoint at line %d stops on hit %s",
      "Breakpoint at line %d stops every time"};
  (void)ctx;
  return setBreakpointText(args, e, &what);
}

/**
 * @brief Lisp function: make the breakpoint at the cursor a logpoint
 * @details (debug-logpoint ["message"]) — asks when it isn't given. The
 * program doesn't stop there: the message goes to the output each time,
 * with every {expression} replaced by its value ("i = {i}"). An empty one
 * makes it an ordinary breakpoint again. Adds the breakpoint if the line
 * has none.
 * @return lisp_null()
 */
Lisp l_debugLogpoint(Lisp args, LispError *e, LispContext ctx) {
  static const BreakpointText what = {
      offsetof(Breakpoint, log_message), breakpointSetLogMessage,
      "Log ({expression}s are replaced): %u (ESC to cancel)", "Line %d logs: %s",
      "Breakpoint at line %d stops again"};
  (void)ctx;
  return setBreakpointText(args, e, &what);
}

/**
 * @brief Lisp function: remove every breakpoint
 * @details (debug-clear-breakpoints)
 * @return how many there were
 */
Lisp l_debugClearBreakpoints(Lisp args, LispError *e, LispContext ctx) {
  int n;
  (void)args;
  (void)e;
  (void)ctx;
  n = breakpointsClear();
  editorSetStatusMessage("Removed %d breakpoint%s", n, n == 1 ? "" : "s");
  return lisp_make_int(n);
}

/**
 * @brief Lisp function: the breakpoints, listed
 * @details (debug-breakpoints) lists them in the "*search*" buffer (ENTER
 * jumps to one) and returns them as a list of ("path" line "condition"
 * "hit-count" "log-message"), line 1-based and "" for each text it doesn't
 * have. (debug-breakpoints #f) only returns the list.
 * @return Lisp list
 */
Lisp l_debugBreakpoints(Lisp args, LispError *e, LispContext ctx) {
  int count;
  int i;
  const Breakpoint *all = breakpointsAll(&count);
  SearchLocation *locs;
  Lisp result = lisp_null();
  Lisp entry;
  (void)e;
  for (i = count - 1; i >= 0; i--) {
    entry = lisp_cons(lisp_make_string2(all[i].log_message ? all[i].log_message : "", ctx),
                      lisp_null(), ctx);
    entry = lisp_cons(lisp_make_string2(all[i].hit_condition ? all[i].hit_condition : "", ctx),
                      entry, ctx);
    entry = lisp_cons(lisp_make_string2(all[i].condition ? all[i].condition : "", ctx), entry,
                      ctx);
    entry = lisp_cons(lisp_make_int(all[i].line + 1), entry, ctx);
    entry = lisp_cons(lisp_make_string2(all[i].path, ctx), entry, ctx);
    result = lisp_cons(entry, result, ctx);
  }
  if (lisp_is_pair(args) && !lisp_is_true(lisp_car(args)))
    return result;
  if (count == 0) {
    editorSetStatusMessage("No breakpoints");
    return result;
  }
  locs = galloc(sizeof(SearchLocation) * (size_t)count);
  if (!locs)
    return result;
  for (i = 0; i < count; i++) {
    locs[i].abs_path = all[i].path;
    locs[i].line = all[i].line + 1;
    locs[i].col = 0;
    locs[i].text = all[i].log_message ? all[i].log_message
                   : all[i].condition ? all[i].condition
                   : all[i].hit_condition ? all[i].hit_condition
                                          : "breakpoint";
  }
  projectSearchShowLocations("Breakpoints", locs, count);
  gfree(locs);
  return result;
}

/**
 * @brief Lisp function: which exceptions stop the program
 * @details (debug-exception-breakpoints) asks, listing the filters the
 * adapter offers (start a session first to see them); (debug-exception-
 * breakpoints "raised" "uncaught") sets them, by the adapter's ids
 * (debugpy: raised, uncaught, userUnhandled; gdb: throw, catch, rethrow;
 * lldb-dap: cpp_throw, cpp_catch). (debug-exception-breakpoints #f) stops
 * on none, (debug-exception-breakpoints #t) goes back to the adapter's
 * defaults. Every session gets them, running or to come.
 * @return the filters on, space-separated
 */
Lisp l_debugExceptionBreakpoints(Lisp args, LispError *e, LispContext ctx) {
  char *ids = NULL;
  char *joined;
  char *offered;
  char *prompt;
  char *p;
  Utf8Char *placeholder;
  Utf8Char *input;
  Lisp it;
  int count;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_BOOL) {
    dapSetExceptionFilters(lisp_is_true(lisp_car(args)) ? NULL : "");
  } else if (lisp_is_pair(args)) {
    for (it = args; lisp_is_pair(it); it = lisp_cdr(it)) {
      if (lisp_type(lisp_car(it)) != LISP_STRING) {
        gfree(ids);
        *e = LISP_ERROR_ARG_TYPE;
        return lisp_null();
      }
      joined = utilFormat("%s%s%s", ids ? ids : "", ids ? " " : "", lisp_string(lisp_car(it)));
      gfree(ids);
      ids = joined;
    }
    dapSetExceptionFilters(ids);
    gfree(ids);
  } else {
    offered = dapExceptionFilters(&count);
    prompt = offered ? utilFormat("Stop on exceptions [%s]: %%u", offered)
                     : utilStrdup("Stop on exceptions (the adapter's filter ids; start a "
                                  "session to see them): %u");
    gfree(offered);
    if (!prompt)
      return lisp_null();
    // A label's own "%" isn't a format.
    for (p = strchr(prompt, '%'); p && p[1] != 'u'; p = strchr(p + 1, '%'))
      *p = ' ';
    ids = dapExceptionChoice();
    placeholder = ids ? stringToUtf8((unsigned char *)ids) : NULL;
    gfree(ids);
    input = editorPromptEx(prompt, placeholder, 0, 1);
    gfree(placeholder);
    gfree(prompt);
    if (!input)
      return lisp_null();
    ids = (char *)utf8ToString(input);
    gfree(input);
    dapSetExceptionFilters(ids ? ids : "");
    gfree(ids);
  }
  ids = dapExceptionChoice();
  editorSetStatusMessage(ids && *ids ? "Exceptions that stop the program: %s"
                                     : "No exception stops the program%s",
                         ids ? (*ids ? ids : "") : "");
  it = lisp_make_string2(ids ? ids : "", ctx);
  gfree(ids);
  return it;
}

// ─── Adapters and the session ───────────────────────────────────────────────

static int isAlist(Lisp l);
static cJSON *lispToJson(Lisp v);

/**
 * @brief Lisp function: register a debug adapter
 * @details (dap-adapter "ext" "program" "arg" ... [launch-defaults]) — the
 * command that debugs programs written in files with extension "ext", e.g.
 * (dap-adapter "c" "gdb" "-i" "dap"). The first registration for an
 * extension wins, like lsp-server. An association list at the end gives
 * launch arguments for every session that doesn't set them itself:
 *   (dap-adapter "c" "xcrun" "lldb-dap" '(("sourceMap" ("/tmp" . "/private/tmp"))))
 * An adapter that listens on a port rather than on stdio gets "{port}" in
 * its arguments, replaced by a free local port that is then connected to:
 *   (dap-adapter "go" "dlv" "dap" "--listen=127.0.0.1:{port}")
 * and one already running is named by its address: (dap-adapter "js"
 * "tcp:127.0.0.1:8123").
 * @return lisp_null()
 */
Lisp l_dapAdapter(Lisp args, LispError *e, LispContext ctx) {
  const char *argv[9];
  const char *ext;
  int argc = 0;
  Lisp it;
  Lisp defaults = lisp_null();
  cJSON *json;
  char *text = NULL;
  (void)ctx;
  if (!lisp_is_pair(args) || !lisp_is_pair(lisp_cdr(args))) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  if (lisp_type(lisp_car(args)) != LISP_STRING) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  for (it = lisp_cdr(args); lisp_is_pair(it); it = lisp_cdr(it)) {
    if (!lisp_is_pair(lisp_cdr(it)) && isAlist(lisp_car(it)))
      defaults = lisp_car(it);
    else if (lisp_type(lisp_car(it)) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
  }
  ext = lisp_string(lisp_car(args));
  for (it = lisp_cdr(args); lisp_is_pair(it) && argc < 8; it = lisp_cdr(it))
    if (lisp_type(lisp_car(it)) == LISP_STRING)
      argv[argc++] = lisp_string(lisp_car(it));
  argv[argc] = NULL;
  if (argc == 0) {
    *e = LISP_ERROR_TOO_FEW_ARGS;
    return lisp_null();
  }
  if (lisp_is_pair(defaults)) {
    json = lispToJson(defaults);
    text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
  }
  if (dapRegisterAdapter(ext, argv, text) != 0)
    editorSetStatusMessage("dap-adapter: too many adapters, ignoring .%s", ext);
  gfree(text);
  return lisp_null();
}

static int isAlist(Lisp l) {
  Lisp key;
  if (!lisp_is_pair(l))
    return 0;
  for (; lisp_is_pair(l); l = lisp_cdr(l)) {
    if (!lisp_is_pair(lisp_car(l)))
      return 0;
    key = lisp_car(lisp_car(l));
    if (lisp_type(key) != LISP_STRING && lisp_type(key) != LISP_SYMBOL)
      return 0;
  }
  return 1;
}

/**
 * @brief A Lisp value as JSON: strings, numbers and booleans as such, an
 * association list (("key" . value) ...) as an object, any other list as
 * an array. NULL for what has no JSON form.
 */
static cJSON *lispToJson(Lisp v) {
  cJSON *out;
  cJSON *item;
  Lisp key;
  switch (lisp_type(v)) {
  case LISP_STRING:
    return cJSON_CreateString(lisp_string(v));
  case LISP_SYMBOL:
    return cJSON_CreateString(lisp_symbol_string(v));
  case LISP_INT:
    return cJSON_CreateNumber((double)lisp_int(v));
  case LISP_REAL:
    return cJSON_CreateNumber((double)lisp_real(v));
  case LISP_BOOL:
    return cJSON_CreateBool(lisp_bool(v));
  case LISP_NULL:
    return cJSON_CreateArray();
  case LISP_PAIR:
    if (isAlist(v)) {
      out = cJSON_CreateObject();
      for (; lisp_is_pair(v); v = lisp_cdr(v)) {
        key = lisp_car(lisp_car(v));
        item = lispToJson(lisp_cdr(lisp_car(v)));
        if (item)
          cJSON_AddItemToObject(out, lisp_type(key) == LISP_STRING ? lisp_string(key)
                                                                   : lisp_symbol_string(key),
                                item);
      }
      return out;
    }
    out = cJSON_CreateArray();
    for (; lisp_is_pair(v); v = lisp_cdr(v)) {
      item = lispToJson(lisp_car(v));
      if (item)
        cJSON_AddItemToArray(out, item);
    }
    return out;
  default:
    return NULL;
  }
}

/**
 * @brief Splits `text` on spaces into a JSON array of strings ("a 'b c'"
 * gives ["a", "b c"]: single or double quotes group words).
 */
static cJSON *splitArgs(const char *text) {
  cJSON *list = cJSON_CreateArray();
  char word[1024];
  size_t n;
  char quote;
  while (*text) {
    while (*text == ' ')
      text++;
    if (!*text)
      break;
    n = 0;
    quote = 0;
    for (; *text && (quote || *text != ' '); text++) {
      if (!quote && (*text == '"' || *text == '\'')) {
        quote = *text;
      } else if (quote && *text == quote) {
        quote = 0;
      } else if (n + 1 < sizeof(word)) {
        word[n++] = *text;
      }
    }
    word[n] = '\0';
    cJSON_AddItemToArray(list, cJSON_CreateString(word));
  }
  return list;
}

// Asks for the program and its arguments. NULL when cancelled.
static cJSON *promptLaunch(void) {
  Utf8Char *placeholder;
  Utf8Char *input;
  char *program;
  char *arguments;
  cJSON *launch;
  placeholder = s_last_program ? stringToUtf8((unsigned char *)s_last_program) : NULL;
  input = editorPrompt("Debug program: %u (ESC to cancel)", placeholder, 1);
  gfree(placeholder);
  if (!input)
    return NULL;
  program = (char *)utf8ToString(input);
  gfree(input);
  if (!program || !*program) {
    gfree(program);
    return NULL;
  }
  placeholder = s_last_args ? stringToUtf8((unsigned char *)s_last_args) : NULL;
  input = editorPromptEx("Arguments: %u (ENTER for none)", placeholder, 0, 1);
  gfree(placeholder);
  if (!input) {
    gfree(program);
    return NULL;
  }
  arguments = (char *)utf8ToString(input);
  gfree(input);
  launch = cJSON_CreateObject();
  cJSON_AddStringToObject(launch, "program", program);
  cJSON_AddItemToObject(launch, "args", splitArgs(arguments ? arguments : ""));
  gfree(s_last_program);
  gfree(s_last_args);
  s_last_program = program;
  s_last_args = arguments;
  return launch;
}

/**
 * @brief Lisp function: start debugging
 * @details (debug-start) asks for the program and its arguments.
 * (debug-start "program" "arg" ...) runs that. (debug-start alist) hands
 * the association list to the adapter as its launch arguments, for
 * whatever else it takes:
 *   (debug-start '(("program" . "build/test_buffer") ("args" "-v")
 *                  ("stopAtBeginningOfMainSubprogram" . #t)))
 * An "adapter" entry ("c", "py", ...) picks the adapter, which otherwise
 * is the active buffer's extension's (or the first registered). "cwd"
 * defaults to the project root, which relative paths start from.
 * @return #t if the session started
 */
Lisp l_debugStart(Lisp args, LispError *e, LispContext ctx) {
  cJSON *launch;
  cJSON *list;
  cJSON *adapter;
  char *ext = NULL;
  char *json;
  int ok;
  Lisp it;
  (void)ctx;
  if (!lisp_is_pair(args)) {
    launch = promptLaunch();
    if (!launch)
      return lisp_false();
  } else if (lisp_type(lisp_car(args)) == LISP_STRING) {
    launch = cJSON_CreateObject();
    cJSON_AddStringToObject(launch, "program", lisp_string(lisp_car(args)));
    list = cJSON_AddArrayToObject(launch, "args");
    for (it = lisp_cdr(args); lisp_is_pair(it); it = lisp_cdr(it)) {
      if (lisp_type(lisp_car(it)) != LISP_STRING) {
        cJSON_Delete(launch);
        *e = LISP_ERROR_ARG_TYPE;
        return lisp_null();
      }
      cJSON_AddItemToArray(list, cJSON_CreateString(lisp_string(lisp_car(it))));
    }
  } else if (isAlist(lisp_car(args))) {
    launch = lispToJson(lisp_car(args));
  } else {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  adapter = cJSON_DetachItemFromObjectCaseSensitive(launch, "adapter");
  if (cJSON_IsString(adapter))
    ext = utilStrdup(adapter->valuestring);
  cJSON_Delete(adapter);
  json = cJSON_PrintUnformatted(launch);
  cJSON_Delete(launch);
  ok = json && dapStart(ext, json) == 0;
  gfree(json);
  gfree(ext);
  return lisp_make_bool(ok);
}

// Attaches to `pid`, with the adapter for the active buffer's file.
static int attachToPid(const long pid) {
  cJSON *args = cJSON_CreateObject();
  char *json;
  int ok;
  // gdb and lldb-dap read "pid", debugpy "processId"; each ignores the other.
  cJSON_AddNumberToObject(args, "pid", (double)pid);
  cJSON_AddNumberToObject(args, "processId", (double)pid);
  json = cJSON_PrintUnformatted(args);
  cJSON_Delete(args);
  ok = json && dapAttach(NULL, json) == 0;
  gfree(json);
  return ok;
}

// The process picked in (debug-attach)'s list.
static Lisp attachSelected(Lisp args, LispError *e, LispContext ctx) {
  (void)e;
  (void)ctx;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT)
    attachToPid((long)lisp_int(lisp_car(args)));
  return lisp_null();
}

/**
 * @brief Lisp function: debug a program that is already running
 * @details (debug-attach) lists your processes to pick one from (typing
 * narrows the list); (debug-attach pid) attaches to that one;
 * (debug-attach alist) hands the adapter its own attach arguments, with an
 * "adapter" entry to pick it as in debug-start. The adapter is the active
 * buffer's file type's. Stopping the session (CTRL-x B q) detaches and
 * leaves the program running. On Linux gdb may need
 * /proc/sys/kernel/yama/ptrace_scope at 0 to attach to a process it didn't
 * start.
 * @return #t if the session started (for the list: once it's open)
 */
Lisp l_debugAttach(Lisp args, LispError *e, LispContext ctx) {
  OsProcessInfo *procs;
  char label[320];
  Lisp items = lisp_null();
  cJSON *attach;
  cJSON *adapter;
  char *ext = NULL;
  char *json;
  int count;
  int ok;
  int i;
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_INT)
    return lisp_make_bool(attachToPid((long)lisp_int(lisp_car(args))));
  if (lisp_is_pair(args)) {
    if (!isAlist(lisp_car(args))) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    attach = lispToJson(lisp_car(args));
    adapter = cJSON_DetachItemFromObjectCaseSensitive(attach, "adapter");
    if (cJSON_IsString(adapter))
      ext = utilStrdup(adapter->valuestring);
    cJSON_Delete(adapter);
    json = cJSON_PrintUnformatted(attach);
    cJSON_Delete(attach);
    ok = json && dapAttach(ext, json) == 0;
    gfree(json);
    gfree(ext);
    return lisp_make_bool(ok);
  }
  procs = os_process_list(&count);
  if (!procs || count == 0) {
    gfree(procs);
    editorSetStatusMessage("Couldn't list the processes");
    return lisp_false();
  }
  // Newest (highest pid) first: usually the one just started.
  for (i = 0; i < count; i++) {
    snprintf(label, sizeof(label), "%7ld  %s", procs[i].pid, procs[i].command);
    items = lisp_cons(lisp_cons(lisp_make_string2(label, ctx), lisp_make_int(procs[i].pid), ctx),
                      items, ctx);
  }
  gfree(procs);
  listPopupOpen("Attach to", items, lisp_make_func(attachSelected), lisp_null(), 1);
  return lisp_true();
}

/**
 * @brief Lisp function: continue the paused program, or start one
 * @details (debug-continue) — what CTRL-x B c does: without a session, it's
 * (debug-start).
 * @return lisp_null()
 */
Lisp l_debugContinue(Lisp args, LispError *e, LispContext ctx) {
  if (dapState() == DAP_OFF)
    return l_debugStart(lisp_null(), e, ctx);
  dapContinue();
  return lisp_null();
}

// (debug-stop), (debug-next), ...: no arguments, one dap.c call each.
#define DEBUG_COMMAND(name, call)                                              \
  Lisp name(Lisp args, LispError *e, LispContext ctx) {                         \
    (void)args;                                                                \
    (void)e;                                                                   \
    (void)ctx;                                                                 \
    call;                                                                      \
    return lisp_null();                                                        \
  }

DEBUG_COMMAND(l_debugStop, dapStop())
DEBUG_COMMAND(l_debugNext, dapNext())
DEBUG_COMMAND(l_debugStepIn, dapStepIn())
DEBUG_COMMAND(l_debugStepOut, dapStepOut())
DEBUG_COMMAND(l_debugPause, dapPause())
DEBUG_COMMAND(l_debugPanel, debugUiOpenPanel())
DEBUG_COMMAND(l_debugPanelSelect, debugUiPanelSelect())
DEBUG_COMMAND(l_debugOutput, debugUiShowOutput())

/**
 * @brief Evaluates the argument, or what the user types, in the debugger.
 */
static Lisp evaluate(Lisp args, LispError *e, const char *prompt, const int repl) {
  Utf8Char *input;
  char *text;
  if (lisp_is_pair(args)) {
    if (lisp_type(lisp_car(args)) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    dapEvaluate(lisp_string(lisp_car(args)), repl);
    return lisp_null();
  }
  input = editorPrompt(prompt, NULL, 0);
  if (!input)
    return lisp_null();
  text = (char *)utf8ToString(input);
  gfree(input);
  if (text && *text)
    dapEvaluate(text, repl);
  gfree(text);
  return lisp_null();
}

/**
 * @brief Lisp function: evaluate an expression in the paused program
 * @details (debug-evaluate ["expression"]) — in the selected frame; asks
 * for it when not given. The result is shown in the status bar and the
 * output.
 * @return lisp_null()
 */
Lisp l_debugEvaluate(Lisp args, LispError *e, LispContext ctx) {
  (void)ctx;
  return evaluate(args, e, "Evaluate: %u (ESC to cancel)", 0);
}

/**
 * @brief Lisp function: a command for the debugger itself
 * @details (debug-command ["command"]) — sent as typed in the adapter's
 * console: for gdb, any gdb command ("info registers", "x/4x $sp").
 * @return lisp_null()
 */
Lisp l_debugCommand(Lisp args, LispError *e, LispContext ctx) {
  (void)ctx;
  return evaluate(args, e, "Debugger command: %u (ESC to cancel)", 1);
}

/**
 * @brief Lisp function: watch an expression
 * @details (debug-watch ["expression"]) — asks when it isn't given. The
 * panel shows its value in the selected frame each time the program stops,
 * in every project's session.
 * @return lisp_null()
 */
Lisp l_debugWatch(Lisp args, LispError *e, LispContext ctx) {
  Utf8Char *input;
  char *text;
  (void)ctx;
  if (lisp_is_pair(args)) {
    if (lisp_type(lisp_car(args)) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    text = utilStrdup(lisp_string(lisp_car(args)));
  } else {
    input = editorPrompt("Watch: %u (ESC to cancel)", NULL, 0);
    if (!input)
      return lisp_null();
    text = (char *)utf8ToString(input);
    gfree(input);
  }
  if (text && *text && dapWatchAdd(text) == 0) {
    debugUiOpenPanel();
    editorSetStatusMessage("Watching %s", text);
  }
  gfree(text);
  return lisp_null();
}

/**
 * @brief Lisp function: stop watching an expression
 * @details (debug-unwatch ["expression"]) — without it, the watch on the
 * panel's row under the cursor, else asks (offering the last one added).
 * @return #t if one was removed
 */
Lisp l_debugUnwatch(Lisp args, LispError *e, LispContext ctx) {
  const char *const *watches;
  Utf8Char *placeholder;
  Utf8Char *input;
  char *text = NULL;
  int count;
  int index = -1;
  int i;
  (void)ctx;
  watches = dapWatches(&count);
  if (lisp_is_pair(args)) {
    if (lisp_type(lisp_car(args)) != LISP_STRING) {
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    text = utilStrdup(lisp_string(lisp_car(args)));
  } else if ((index = debugUiPanelWatch()) < 0) {
    if (count == 0) {
      editorSetStatusMessage("No watch expressions");
      return lisp_false();
    }
    placeholder = stringToUtf8((unsigned char *)watches[count - 1]);
    input = editorPrompt("Stop watching: %u (ESC to cancel)", placeholder, 0);
    gfree(placeholder);
    if (!input)
      return lisp_false();
    text = (char *)utf8ToString(input);
    gfree(input);
  }
  for (i = 0; text && i < count && index < 0; i++)
    if (!strcmp(watches[i], text))
      index = i;
  gfree(text);
  if (index < 0) {
    editorSetStatusMessage("Not watched");
    return lisp_false();
  }
  editorSetStatusMessage("Stopped watching %s", watches[index]);
  return lisp_make_bool(dapWatchRemove(index) == 0);
}

/**
 * @brief Lisp function: the watch expressions
 * @details (debug-watches) — a list of ("expression" value), value being
 * what it came to in the paused program, #f when it isn't paused.
 * @return Lisp list
 */
Lisp l_debugWatches(Lisp args, LispError *e, LispContext ctx) {
  const char *const *watches;
  const char *value;
  Lisp result = lisp_null();
  Lisp entry;
  int count;
  int i;
  (void)args;
  (void)e;
  watches = dapWatches(&count);
  for (i = count - 1; i >= 0; i--) {
    value = dapWatchValue(i);
    entry = lisp_cons(value ? lisp_make_string2(value, ctx) : lisp_false(), lisp_null(), ctx);
    entry = lisp_cons(lisp_make_string2(watches[i], ctx), entry, ctx);
    result = lisp_cons(entry, result, ctx);
  }
  return result;
}

/**
 * @brief Lisp function: the session's state
 * @details (debug-state) — "off", "starting", "running" or "stopped".
 * @return Lisp string
 */
Lisp l_debugState(Lisp args, LispError *e, LispContext ctx) {
  static const char *const names[] = {"off", "starting", "running", "stopped"};
  (void)args;
  (void)e;
  return lisp_make_string2(names[dapState()], ctx);
}

/**
 * @brief Lisp function: the paused program's call stack
 * @details (debug-frames) — a list of ("function" "path" line), innermost
 * first, line 1-based; path is #f for code without a source file. Empty
 * unless the program is stopped.
 * @return Lisp list
 */
Lisp l_debugFrames(Lisp args, LispError *e, LispContext ctx) {
  int count;
  int selected;
  int i;
  const DapFrame *frames = dapFrames(&count, &selected);
  Lisp result = lisp_null();
  Lisp entry;
  (void)args;
  (void)e;
  for (i = count - 1; i >= 0; i--) {
    entry = lisp_cons(lisp_make_int(frames[i].line + 1), lisp_null(), ctx);
    entry = lisp_cons(frames[i].path ? lisp_make_string2(frames[i].path, ctx) : lisp_false(),
                      entry, ctx);
    entry = lisp_cons(lisp_make_string2(frames[i].name ? frames[i].name : "", ctx), entry, ctx);
    result = lisp_cons(entry, result, ctx);
  }
  return result;
}

void debugBuiltinsFree(void) {
  gfree(s_last_program);
  gfree(s_last_args);
  s_last_program = NULL;
  s_last_args = NULL;
}
