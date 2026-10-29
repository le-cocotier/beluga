#include "../include/lisp_gc.h"
#include "../include/async_process.h"
#include "../include/daemon.h"

extern EditorConfig E;

// Collect once the heap has twice the size it had after the last
// collection, and never below this.
#define LISP_GC_MIN_HEAP (8 * 1024 * 1024)

static size_t s_collect_at = LISP_GC_MIN_HEAP;

void lispGcVisitListPopup(ListPopup *popup, LispRootVisitor visit, LispContext ctx) {
  if (!popup->visible) {
    // Left from the last time it was up: the objects may be gone.
    popup->items = lisp_null();
    popup->all_items = lisp_null();
    popup->on_select = lisp_null();
    popup->on_cancel = lisp_null();
    return;
  }
  visit(&popup->items, ctx);
  visit(&popup->all_items, ctx);
  visit(&popup->on_select, ctx);
  visit(&popup->on_cancel, ctx);
}

static void visitRoots(LispRootVisitor visit, LispContext ctx) {
  Buffer *buf;
  int i;

  for (i = 0; i < E.number_of_keybinds; i++)
    visit(&E.key_binds[i].command, ctx);
  for (i = 0; i < E.number_of_hooks; i++)
    visit(&E.hooks[i].callback, ctx);
  for (i = 0; i < E.number_of_projects; i++) {
    if (E.projects[i].has_config_env)
      visit(&E.projects[i].config_env, ctx);
    else
      E.projects[i].config_env = lisp_null();
  }
  buf = E.buffers; // a ring
  if (buf)
    do {
      visit(&buf->locals, ctx);
      buf = buf->next;
    } while (buf && buf != E.buffers);
  visit(&E.ctx_data, ctx);
  lispGcVisitListPopup(&E.list_popup, visit, ctx);
  asyncProcessVisitLispRoots(visit, ctx);
  daemonVisitLispRoots(visit, ctx);
}

void lispGcInit(void) { lisp_set_roots(visitRoots, E.ctx); }

void lispGcMaybeCollect(void) {
  size_t live;
  if (!E.ctx.p)
    return;
#ifndef BELUGA_LISP_GC_STRESS // collect every time, to test the roots
  if (lisp_heap_size(E.ctx) < s_collect_at)
    return;
#endif
  lisp_collect(lisp_null(), E.ctx);
  live = lisp_heap_size(E.ctx);
  s_collect_at = live * 2 > LISP_GC_MIN_HEAP ? live * 2 : LISP_GC_MIN_HEAP;
}
