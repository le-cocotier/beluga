#ifndef BELUGA_DESCRIBE_H
#define BELUGA_DESCRIBE_H

#include "data.h"

// Self-documentation (describe.c): what a key runs, what is bound where.

// (describe-key): reads a key sequence, following prefixes (CTRL-x ...),
// and says in the status bar what it runs.
Lisp describeKey(Lisp args, LispError *e, LispContext ctx);
// (key-bindings): every binding, as a list of (prefix key command)
// strings, in the order they're matched. describe-bindings (init.lisp)
// shows it in a buffer.
Lisp keyBindings(Lisp args, LispError *e, LispContext ctx);
// (describe-function name): whether `name` is a built-in or a Lisp
// function (with its definition), and the keys bound to it, as a list of
// lines for a help buffer.
Lisp describeFunction(Lisp args, LispError *e, LispContext ctx);

#endif // BELUGA_DESCRIBE_H
