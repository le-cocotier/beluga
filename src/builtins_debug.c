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
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"
#include "../include/utils.h"

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
  char *path;
  int line;
  Utf8Char *input;
  char *condition;
  const Breakpoint *bp;
  Utf8Char *current;
  (void)ctx;
  if (!cursorLocation(&path, &line))
    return lisp_null();
  if (lisp_is_pair(args)) {
    if (lisp_type(lisp_car(args)) != LISP_STRING) {
      gfree(path);
      *e = LISP_ERROR_ARG_TYPE;
      return lisp_null();
    }
    condition = utilStrdup(lisp_string(lisp_car(args)));
  } else {
    bp = breakpointAt(path, line);
    current = (bp && bp->condition) ? stringToUtf8((unsigned char *)bp->condition) : NULL;
    input = editorPrompt("Break when: %u (ESC to cancel)", current, 0);
    gfree(current);
    if (!input) {
      gfree(path);
      return lisp_null();
    }
    condition = (char *)utf8ToString(input);
    gfree(input);
  }
  if (breakpointSetCondition(path, line, condition) == 0)
    editorSetStatusMessage(condition && *condition ? "Breakpoint at line %d stops when %s"
                                                   : "Breakpoint at line %d always stops",
                           line + 1, condition);
  gfree(condition);
  gfree(path);
  return lisp_null();
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
 * jumps to one) and returns them as a list of ("path" line "condition"),
 * line 1-based and condition "" for none. (debug-breakpoints #f) only
 * returns the list.
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
    entry = lisp_cons(lisp_make_string2(all[i].condition ? all[i].condition : "", ctx),
                      lisp_null(), ctx);
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
    locs[i].text = all[i].condition ? all[i].condition : "breakpoint";
  }
  projectSearchShowLocations("Breakpoints", locs, count);
  gfree(locs);
  return result;
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
