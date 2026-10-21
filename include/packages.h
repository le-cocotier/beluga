#ifndef PACKAGES_H
#define PACKAGES_H

#include "lisp.h"

// ─── Packages ────────────────────────────────────────────────────────────────
// Loading a package: (add-package "name" ["dir"]) evaluates dir/init.lisp
// (by default <beluga dir>/packages/name) once per session, after loading
// what its optional package.lisp manifest requires. Installing, updating
// and removing packages is Lisp, in the bundled "package" package; this is
// only what has to be C: load order, the per-package state, and version
// comparison.

// Compares dotted version strings numerically ("1.10" > "1.9", "v2" ==
// "2.0", "1.2.0-rc1" == "1.2.0"): <0, 0 or >0 like strcmp.
int packageVersionCompare(const char *a, const char *b);

// Frees the table of loaded packages (deInitEditor).
void packagesFree(void);

Lisp addPackage(Lisp args, LispError *e, LispContext ctx);
Lisp packageLoaded(Lisp args, LispError *e, LispContext ctx);
Lisp l_packageState(Lisp args, LispError *e, LispContext ctx);
Lisp l_packageVersion(Lisp args, LispError *e, LispContext ctx);
Lisp l_belugaVersion(Lisp args, LispError *e, LispContext ctx);
Lisp l_versionCompare(Lisp args, LispError *e, LispContext ctx);
Lisp l_belugaDirectory(Lisp args, LispError *e, LispContext ctx);
Lisp l_packageDirectory(Lisp args, LispError *e, LispContext ctx);
Lisp l_readDataFile(Lisp args, LispError *e, LispContext ctx);
Lisp l_writeDataFile(Lisp args, LispError *e, LispContext ctx);

#endif
