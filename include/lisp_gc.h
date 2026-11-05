#ifndef BELUGA_LISP_GC_H
#define BELUGA_LISP_GC_H

#include "data.h"

// ─── Collecting the Lisp heap ────────────────────────────────────────────────
// Every Lisp evaluation allocates (a call's environment, its argument list,
// the strings it builds), and the interpreter only frees by collecting: it
// copies what is still reachable to a new heap and drops the old one. The
// collector moves objects, so the editor's own Lisp values (key bindings,
// hooks, the list popup, buffer locals, ...) are handed to it as roots to be
// updated (visitRoots() in lisp_gc.c), and it only runs between two ticks of
// a run loop (editorRunLoop(), daemonRunLoop()), when no Lisp value sits in
// a C local.

// Registers the roots with the interpreter. Call once lisp_lib_load() has
// run.
void lispGcInit(void);

// Collects if the heap has grown enough since the last collection. Call
// only at the top level of a run loop: not from anything Lisp can call,
// and with no Lisp value held in a C local.
void lispGcMaybeCollect(void);

// The list popup's values, for a window's saved copy of it (daemon.c). A
// popup that isn't visible holds none; its stale values are cleared.
void lispGcVisitListPopup(ListPopup *popup, LispRootVisitor visit, LispContext ctx);

#endif // BELUGA_LISP_GC_H
