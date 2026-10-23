/**
 * @file builtins_debug.c
 * @brief The debugger's Lisp builtins: breakpoints
 */

#include "../include/builtins.h"

#include "../include/breakpoints.h"
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
