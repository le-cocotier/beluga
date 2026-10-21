/**
 * \file packages.c
 * \brief Loading packages: add-package, the manifest's requires, the
 * per-package load state, version comparison, and the data-file reader and
 * writer the package manager keeps its lockfile with.
 */

#include "../include/packages.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/terminal.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PACKAGE_LOADING = 1, PACKAGE_LOADED, PACKAGE_FAILED };

// One entry per package add-package has tried to load this session.
// "loading" is set before the package's requires and init.lisp are
// evaluated, which is what stops a package that (directly or not)
// requires itself from recursing; "failed" lets a later add-package try
// again (after use-package installed what was missing, say).
typedef struct {
  char *name;
  char *dir;     // where it was loaded from
  char *version; // from its manifest; NULL when it has none
  int state;
} PackageEntry;

static PackageEntry *s_packages = NULL;
static int s_package_count = 0;
static int s_package_cap = 0;

static char *dupString(const char *s) {
  char *out;
  if (!s)
    return NULL;
  out = galloc(strlen(s) + 1);
  if (out)
    strcpy(out, s);
  return out;
}

static PackageEntry *findPackage(const char *name) {
  int i;
  for (i = 0; i < s_package_count; i++)
    if (strcmp(s_packages[i].name, name) == 0)
      return &s_packages[i];
  return NULL;
}

static PackageEntry *addEntry(const char *name) {
  PackageEntry *grown;
  PackageEntry *entry;
  int cap;
  if (s_package_count == s_package_cap) {
    cap = s_package_cap ? s_package_cap * 2 : 32;
    grown = grealloc(s_packages, (size_t)cap * sizeof(PackageEntry));
    if (!grown)
      return NULL;
    s_packages = grown;
    s_package_cap = cap;
  }
  entry = &s_packages[s_package_count++];
  entry->name = dupString(name);
  entry->dir = NULL;
  entry->version = NULL;
  entry->state = 0;
  return entry;
}

static void setString(char **field, const char *value) {
  gfree(*field);
  *field = dupString(value);
}

void packagesFree(void) {
  int i;
  for (i = 0; i < s_package_count; i++) {
    gfree(s_packages[i].name);
    gfree(s_packages[i].dir);
    gfree(s_packages[i].version);
  }
  gfree(s_packages);
  s_packages = NULL;
  s_package_count = 0;
  s_package_cap = 0;
}

// ─── Versions ────────────────────────────────────────────────────────────────

// The next dotted component of a version, advancing *p past it; -1 once
// there are none left (end of string, or a suffix like "-rc1").
static long nextComponent(const char **p) {
  long n = 0;
  if (**p == '.')
    (*p)++;
  if (**p < '0' || **p > '9')
    return -1;
  while (**p >= '0' && **p <= '9') {
    n = n * 10 + (**p - '0');
    (*p)++;
  }
  return n;
}

int packageVersionCompare(const char *a, const char *b) {
  long x;
  long y;
  if (*a == 'v' || *a == 'V')
    a++;
  if (*b == 'v' || *b == 'V')
    b++;
  for (;;) {
    x = nextComponent(&a);
    y = nextComponent(&b);
    if (x < 0 && y < 0)
      return 0;
    // A missing component counts as 0: "2" == "2.0".
    if (x < 0)
      x = 0;
    if (y < 0)
      y = 0;
    if (x != y)
      return x < y ? -1 : 1;
  }
}

// ─── Data files ──────────────────────────────────────────────────────────────

/**
 * @brief Every form in a file, as a list, without evaluating any of them;
 * #f when the file can't be read or doesn't parse.
 * @details The text is read wrapped in one pair of parentheses, so that
 * one form and several come back the same way (lisp_read_file turns
 * several into a (begin ...)). The newline before the closing one keeps a
 * comment on the last line from swallowing it.
 */
static Lisp readDataFile(const char *path, LispContext ctx) {
  FILE *f;
  char *text;
  long size;
  size_t got;
  Lisp forms;
  LispError error = LISP_ERROR_NONE;

  f = fopen(path, "rb");
  if (!f)
    return lisp_make_bool(0);
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return lisp_make_bool(0);
  }
  text = galloc((size_t)size + 4);
  if (!text) {
    fclose(f);
    return lisp_make_bool(0);
  }
  text[0] = '(';
  got = fread(text + 1, 1, (size_t)size, f);
  fclose(f);
  text[1 + got] = '\n';
  text[2 + got] = ')';
  text[3 + got] = '\0';
  forms = lisp_read(text, &error, ctx);
  gfree(text);
  if (error != LISP_ERROR_NONE)
    return lisp_make_bool(0);
  return forms;
}

static void writeEscaped(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    switch (*s) {
    case '"':
      fputs("\\\"", f);
      break;
    case '\\':
      fputs("\\\\", f);
      break;
    case '\n':
      fputs("\\n", f);
      break;
    case '\t':
      fputs("\\t", f);
      break;
    default:
      fputc(*s, f);
    }
  }
  fputc('"', f);
}

// Writes data back in a form readDataFile() reads: lists, strings,
// symbols, integers and booleans; anything else becomes #f.
static void writeDatum(FILE *f, Lisp l) {
  switch (lisp_type(l)) {
  case LISP_NULL:
    fputs("()", f);
    break;
  case LISP_STRING:
    writeEscaped(f, lisp_string(l));
    break;
  case LISP_SYMBOL:
    fputs(lisp_symbol_string(l), f);
    break;
  case LISP_INT:
    fprintf(f, "%lld", (long long)lisp_int(l));
    break;
  case LISP_BOOL:
    fputs(lisp_is_true(l) ? "#t" : "#f", f);
    break;
  case LISP_PAIR:
    fputc('(', f);
    for (;;) {
      writeDatum(f, lisp_car(l));
      l = lisp_cdr(l);
      if (lisp_type(l) != LISP_PAIR)
        break;
      fputc(' ', f);
    }
    if (!lisp_is_null(l)) {
      fputs(" . ", f);
      writeDatum(f, l);
    }
    fputc(')', f);
    break;
  default:
    fputs("#f", f);
  }
}

// ─── The manifest ────────────────────────────────────────────────────────────

// (define-package "name" (field value ...) ...): the (field value ...) list
// for `field`, or the empty list.
static Lisp manifestField(Lisp manifest, const char *field) {
  Lisp it;
  Lisp clause;
  for (it = manifest; lisp_is_pair(it); it = lisp_cdr(it)) {
    clause = lisp_car(it);
    if (lisp_is_pair(clause) && lisp_type(lisp_car(clause)) == LISP_SYMBOL &&
        strcmp(lisp_symbol_string(lisp_car(clause)), field) == 0)
      return lisp_cdr(clause);
  }
  return lisp_null();
}

static const char *manifestString(Lisp manifest, const char *field) {
  Lisp values = manifestField(manifest, field);
  if (lisp_is_pair(values) && lisp_type(lisp_car(values)) == LISP_STRING)
    return lisp_string(lisp_car(values));
  return NULL;
}

// The fields of dir/package.lisp's define-package form; the empty list
// when there is no manifest (or no define-package in it).
static Lisp readManifest(const char *dir, LispContext ctx) {
  char *path = utilFormat("%s/package.lisp", dir);
  Lisp forms;
  Lisp form;
  if (!path)
    return lisp_null();
  forms = readDataFile(path, ctx);
  free(path);
  for (; lisp_is_pair(forms); forms = lisp_cdr(forms)) {
    form = lisp_car(forms);
    if (lisp_is_pair(form) && lisp_type(lisp_car(form)) == LISP_SYMBOL &&
        strcmp(lisp_symbol_string(lisp_car(form)), "define-package") == 0 &&
        lisp_is_pair(lisp_cdr(form)))
      return lisp_cdr(lisp_cdr(form)); // skip the name
  }
  return lisp_null();
}

static int fileExists(const char *dir, const char *name) {
  OsFileInfo info;
  char *path = utilFormat("%s/%s", dir, name);
  int exists;
  if (!path)
    return 0;
  exists = os_file_info(path, &info) == 0;
  free(path);
  return exists;
}

// ─── Loading ─────────────────────────────────────────────────────────────────

static int loadPackage(const char *name, const char *dir_arg, LispContext ctx);

/**
 * @brief Loads what the manifest requires, in order.
 * @return 0 when the package must not load: a dependency is missing or
 * failed, or it needs a newer beluga. A dependency older than required,
 * or a cycle, is only reported.
 */
static int loadRequires(const char *name, Lisp requires, LispContext ctx) {
  Lisp req;
  const char *dep;
  const char *min;
  PackageEntry *entry;

  for (; lisp_is_pair(requires); requires = lisp_cdr(requires)) {
    req = lisp_car(requires);
    if (!lisp_is_pair(req) || lisp_type(lisp_car(req)) != LISP_STRING)
      continue;
    dep = lisp_string(lisp_car(req));
    min = lisp_is_pair(lisp_cdr(req)) && lisp_type(lisp_car(lisp_cdr(req))) == LISP_STRING
              ? lisp_string(lisp_car(lisp_cdr(req)))
              : NULL;

    if (strcmp(dep, "beluga") == 0) {
      if (min && packageVersionCompare(BELUGA_VERSION, min) < 0) {
        editorSetStatusMessage("add-package: \"%s\" needs beluga %s or later (this is %s)", name,
                               min, BELUGA_VERSION);
        appDebug("[PACKAGE] %s: needs beluga %s\n", name, min);
        return 0;
      }
      continue;
    }

    entry = findPackage(dep);
    if (entry && entry->state == PACKAGE_LOADING) {
      editorSetStatusMessage("add-package: \"%s\" and \"%s\" require each other", name, dep);
      appDebug("[PACKAGE] cycle: %s -> %s\n", name, dep);
      continue;
    }
    if (!loadPackage(dep, NULL, ctx)) {
      editorSetStatusMessage("add-package: \"%s\" needs \"%s\", which %s", name, dep,
                             findPackage(dep) ? "failed to load" : "isn't installed");
      appDebug("[PACKAGE] %s: dependency %s missing or failed\n", name, dep);
      return 0;
    }
    entry = findPackage(dep);
    if (min && entry && entry->version && packageVersionCompare(entry->version, min) < 0) {
      editorSetStatusMessage("add-package: \"%s\" wants \"%s\" %s or later (installed: %s)", name,
                             dep, min, entry->version);
      appDebug("[PACKAGE] %s: %s is %s, wants %s\n", name, dep, entry->version, min);
    }
  }
  return 1;
}

/**
 * @brief Loads one package, once: its requires first, then its init.lisp.
 * @param dir_arg the package's directory, or NULL for
 * <beluga dir>/packages/<name>
 * @return 1 when it is loaded (now or before), 0 otherwise
 */
static int loadPackage(const char *name, const char *dir_arg, LispContext ctx) {
  PackageEntry *entry = findPackage(name);
  char *dir;
  char *init_path;
  FILE *fd_package;
  Lisp manifest;
  Lisp code;
  const char *version;
  LispError package_error = LISP_ERROR_NONE;
  int ok = 0;

  if (entry && (entry->state == PACKAGE_LOADED || entry->state == PACKAGE_LOADING))
    return 1;

  if (dir_arg)
    dir = utilFormat("%s", dir_arg);
  else
    dir = utilFormat("%s/packages/%s", utilBelugaDir(), name);
  if (!dir)
    return 0;
  os_path_to_slashes(dir);
  init_path = utilFormat("%s/init.lisp", dir);
  if (!init_path) {
    free(dir);
    return 0;
  }

  fd_package = fopen(init_path, "r");
  if (!fd_package) {
    // personal-config is optional: having none isn't worth a message.
    if (strcmp(name, "personal-config") != 0)
      editorSetStatusMessage("add-package: \"%s\" isn't installed (no %s)", name, init_path);
    appDebug("[PACKAGE] %s: can't open %s\n", name, init_path);
    free(init_path);
    free(dir);
    return 0;
  }

  if (!entry)
    entry = addEntry(name);
  if (!entry) {
    fclose(fd_package);
    free(init_path);
    free(dir);
    return 0;
  }
  entry->state = PACKAGE_LOADING;
  setString(&entry->dir, dir);

  manifest = readManifest(dir, ctx);
  version = manifestString(manifest, "version");
  // A bundled package without a version of its own is as new as the
  // editor it was installed with.
  if (!version && fileExists(dir, ".bundled"))
    version = BELUGA_VERSION;
  setString(&entry->version, version);

  if (loadRequires(name, manifestField(manifest, "requires"), ctx)) {
    // A local error, so a broken package is reported and init.lisp carries
    // on with the next form.
    code = lisp_read_file(fd_package, &package_error, ctx);
    if (package_error == LISP_ERROR_NONE)
      lisp_eval(code, &package_error, ctx);
    if (package_error != LISP_ERROR_NONE) {
      editorSetStatusMessage("add-package: error in \"%s\": %s", name,
                             lisp_error_string(package_error));
      appDebug("[PACKAGE] %s: %s\n", name, lisp_error_string(package_error));
    } else {
      ok = 1;
    }
  }
  fclose(fd_package);
  free(init_path);
  free(dir);

  // The table may have grown (and moved) while the requires loaded.
  entry = findPackage(name);
  if (entry)
    entry->state = ok ? PACKAGE_LOADED : PACKAGE_FAILED;
  return ok;
}

// ─── Lisp functions ──────────────────────────────────────────────────────────

static const char *stringArg(Lisp args, int index) {
  while (index-- > 0 && lisp_is_pair(args))
    args = lisp_cdr(args);
  if (lisp_is_pair(args) && lisp_type(lisp_car(args)) == LISP_STRING)
    return lisp_string(lisp_car(args));
  return NULL;
}

/**
 * @brief Lisp function: (add-package "name" ["dir"]) — load a package once
 * @details Evaluates dir/init.lisp, dir being by default
 * <beluga dir>/packages/name (~/.beluga/packages/name unless BELUGA_HOME
 * says otherwise). Naming a package that's already loaded again does
 * nothing, like Emacs' require. When dir has a package.lisp manifest, what
 * its (requires ...) names is loaded first, and a package needing a newer
 * beluga, or a missing dependency, isn't loaded at all. A missing package
 * and a Lisp error inside one are reported in the status bar.
 * @return #t when the package is loaded, #f when it isn't installed,
 * failed, or couldn't be loaded
 */
Lisp addPackage(const Lisp args, LispError *e, LispContext ctx) {
  const char *name = stringArg(args, 0);
  if (!name) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  appDebug("[PACKAGE] add-package %s\n", name);
  return lisp_make_bool(loadPackage(name, stringArg(args, 1), ctx));
}

/**
 * @brief Lisp function: (package-loaded? "name") — #t once add-package has
 * loaded that package in this session (or while it is loading).
 */
Lisp packageLoaded(const Lisp args, LispError *e, LispContext ctx) {
  const char *name = stringArg(args, 0);
  PackageEntry *entry = name ? findPackage(name) : NULL;
  (void)e;
  (void)ctx;
  return lisp_make_bool(entry &&
                        (entry->state == PACKAGE_LOADED || entry->state == PACKAGE_LOADING));
}

/**
 * @brief Lisp function: (package-state "name") — "loaded", "loading" or
 * "failed", or #f when add-package hasn't tried it this session.
 */
Lisp l_packageState(const Lisp args, LispError *e, LispContext ctx) {
  const char *name = stringArg(args, 0);
  PackageEntry *entry = name ? findPackage(name) : NULL;
  (void)e;
  if (!entry || !entry->state)
    return lisp_make_bool(0);
  return lisp_make_string2(entry->state == PACKAGE_LOADED    ? "loaded"
                           : entry->state == PACKAGE_LOADING ? "loading"
                                                             : "failed",
                           ctx);
}

/**
 * @brief Lisp function: (package-version "name") — the version a loaded
 * package's manifest gave (beluga's own for a bundled package without
 * one), or #f.
 */
Lisp l_packageVersion(const Lisp args, LispError *e, LispContext ctx) {
  const char *name = stringArg(args, 0);
  PackageEntry *entry = name ? findPackage(name) : NULL;
  (void)e;
  if (!entry || !entry->version)
    return lisp_make_bool(0);
  return lisp_make_string2(entry->version, ctx);
}

/**
 * @brief Lisp function: (beluga-version) — "3.3", the running editor's.
 */
Lisp l_belugaVersion(const Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  return lisp_make_string2(BELUGA_VERSION, ctx);
}

/**
 * @brief Lisp function: (version-compare "a" "b") — -1, 0 or 1, comparing
 * dotted versions numerically: "1.10" is newer than "1.9", a leading "v"
 * and a suffix like "-rc1" are ignored.
 */
Lisp l_versionCompare(const Lisp args, LispError *e, LispContext ctx) {
  const char *a = stringArg(args, 0);
  const char *b = stringArg(args, 1);
  int c;
  (void)ctx;
  if (!a || !b) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  c = packageVersionCompare(a, b);
  return lisp_make_int(c < 0 ? -1 : c > 0 ? 1 : 0);
}

/**
 * @brief Lisp function: (beluga-directory) — where beluga keeps its config
 * and packages: $BELUGA_HOME, or ~/.beluga.
 */
Lisp l_belugaDirectory(const Lisp args, LispError *e, LispContext ctx) {
  (void)args;
  (void)e;
  return lisp_make_string2(utilBelugaDir(), ctx);
}

/**
 * @brief Lisp function: (package-directory ["name"]) — the packages
 * directory, or that package's directory in it.
 */
Lisp l_packageDirectory(const Lisp args, LispError *e, LispContext ctx) {
  const char *name = stringArg(args, 0);
  char *path;
  Lisp result;
  (void)e;
  path = name ? utilFormat("%s/packages/%s", utilBelugaDir(), name)
              : utilFormat("%s/packages", utilBelugaDir());
  if (!path)
    return lisp_make_bool(0);
  result = lisp_make_string2(path, ctx);
  free(path);
  return result;
}

/**
 * @brief Lisp function: (read-data-file "path") — the forms in a file, as
 * a list, read but never evaluated; #f when it can't be read or parsed.
 * @details For data written by write-data-file (the package lockfile) or
 * by hand (package manifests): reading a package's manifest must not run
 * any of its code.
 */
Lisp l_readDataFile(const Lisp args, LispError *e, LispContext ctx) {
  const char *path = stringArg(args, 0);
  if (!path) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  return readDataFile(path, ctx);
}

/**
 * @brief Lisp function: (write-data-file "path" forms ["header"]) — writes
 * each of `forms` on a line of its own, after `header` (a comment, say),
 * so read-data-file gives the same list back.
 * @details Written to path.tmp first and renamed over path, so a crash or a
 * full disk never leaves half a file. Lists, strings, symbols, integers
 * and booleans only.
 * @return #t, or #f when the file couldn't be written
 */
Lisp l_writeDataFile(const Lisp args, LispError *e, LispContext ctx) {
  const char *path = stringArg(args, 0);
  const char *header = stringArg(args, 2);
  Lisp forms;
  char *tmp;
  FILE *f;
  int ok;
  (void)ctx;
  if (!path || !lisp_is_pair(lisp_cdr(args))) {
    *e = LISP_ERROR_ARG_TYPE;
    return lisp_null();
  }
  forms = lisp_car(lisp_cdr(args));
  tmp = utilFormat("%s.tmp", path);
  if (!tmp)
    return lisp_make_bool(0);
  f = fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return lisp_make_bool(0);
  }
  if (header)
    fputs(header, f);
  for (; lisp_is_pair(forms); forms = lisp_cdr(forms)) {
    writeDatum(f, lisp_car(forms));
    fputc('\n', f);
  }
  ok = !ferror(f);
  ok = fclose(f) == 0 && ok;
  // rename() won't replace an existing file on Windows.
  if (ok && rename(tmp, path) != 0) {
    remove(path);
    ok = rename(tmp, path) == 0;
  }
  if (!ok)
    remove(tmp);
  free(tmp);
  return lisp_make_bool(ok);
}
