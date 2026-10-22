#ifndef BELUGA_SETTINGS_H
#define BELUGA_SETTINGS_H

#include "data.h"

#include <stddef.h>

// The settings init.lisp can (define ...), in one table (settings.c):
// name, type, default and what each does. Adding a setting is one entry
// there and a field in struct const_t.

// Reads every setting from the Lisp environment into E.constantes, after
// init.lisp has loaded. A missing setting takes its default; one of the
// wrong type (or out of range) takes it too.
void settingsLoad(void);
// The same, from `env` into `out`: a project's env (project.c) gives its
// .beluga/init.lisp's values, and init.lisp's for anything it doesn't
// define. Names of invalid settings are appended to `invalid` if not NULL.
void settingsRead(Lisp env, struct const_t *out, char *invalid, size_t invalid_size);
// Says in the status bar which settings were invalid, if any (main.c,
// once the startup message is up).
void settingsReportProblems(void);

// (describe-variable "NAME"): lines describing a setting (or any variable).
Lisp describeVariable(Lisp args, LispError *e, LispContext ctx);
// (settings): one line per setting — name, current value, default, doc.
Lisp settingsList(Lisp args, LispError *e, LispContext ctx);

#endif // BELUGA_SETTINGS_H
