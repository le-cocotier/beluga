#ifndef BELUGA_PROJECT_H_
#define BELUGA_PROJECT_H_

#include "data.h"

// ─── Projects ───────────────────────────────────────────────────────────────
// A project is the nearest directory above a file that holds one of the
// project markers (.beluga, .git, compile_commands.json, plus whatever
// (project-add-marker ...) adds). Every project beluga has seen is
// remembered in ~/.beluga/projects, most recently used first.
//
// The list of projects belongs to the whole process; which one is
// "current" follows the active pane's buffer. A buffer without a project
// (a terminal, *search*, a file outside any project) leaves the current
// project as it was.
//
// A project may carry its own config, <root>/.beluga/init.lisp. It is
// only evaluated once the project is trusted (project-trust), since a
// cloned repository could otherwise run any Lisp, shell-run included. It
// runs in its own environment on top of the global one: a (define ...)
// there, TAB-LENGTH or LINE-NUMBERS for instance, only applies while one
// of the project's buffers is active.

// The directory holding a project's own config, and the file inside it.
#define PROJECT_CONFIG_DIR ".beluga"
#define PROJECT_CONFIG_FILE "init.lisp"

// Registers the project-* builtins. Called from initBuiltins(), before
// init.lisp loads, so the global config can already add markers.
void projectRegisterBuiltins(void);

// Loads ~/.beluga/projects and makes the working directory's project
// current. Called once init.lisp has been evaluated and E.constantes read.
void projectInit(void);

// Frees the project list. Called from deInitEditor().
void projectFree(void);

// Resolves `buffer`'s project (once) and makes it current. editorOpen()
// calls it before a file is read, so the file already loads with its
// project's settings (TAB-LENGTH expands tabs while loading).
void projectActivateForBuffer(Buffer *buffer);

// Follows the active pane: makes its buffer's project current. Cheap when
// nothing changed; called once per editorRunLoop() tick.
void projectSyncActive(void);

// Trades the buffer projectSyncActive() last saw for `other`: each daemon
// window has its own current project (daemon.c).
void projectSwapLastBuffer(int *other);

// The current project's root, or NULL when there is none. Owned by the
// project list.
const char *projectCurrentRoot(void);

// The current project's root, or the working directory without one, as a
// galloc'd string (free with gfree). NULL only if both are unavailable.
// What find-file, project-search, the open-file prompt and new terminals
// start from.
char *projectRootOrCwd(void);

#endif // BELUGA_PROJECT_H_
