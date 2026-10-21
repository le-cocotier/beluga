#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdlib.h>

void* recalloc(void* ptr, size_t old_size, size_t new_size);

// ─── Portable string helpers ────────────────────────────────────────────────
// The editor is plain C99 plus os.h: these stand in for the POSIX/GNU
// extensions it used (strdup, strndup, strtok_r, memmem, asprintf), which
// not every C library provides. All allocate with galloc (gfree() them).

char *utilStrdup(const char *s);
char *utilStrndup(const char *s, size_t n);
// strtok_r(): splits `str` on `delim`, in place.
char *utilStrtok(char *str, const char *delim, char **saveptr);
void *utilMemmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len);
// asprintf(): the formatted string, galloc'd; NULL on failure.
char *utilFormat(const char *fmt, ...);

// The directory part of a '/'-separated path, written into `out`
// ("a/b/c.txt" -> "a/b", "c.txt" -> ".", "/c.txt" -> "/", "C:/x" -> "C:/").
void utilDirname(const char *path, char *out, size_t out_size);

// Where beluga keeps its config, packages and logs: $BELUGA_HOME when set
// (tests, a portable install), else ~/.beluga. Never NULL once HOME is.
const char *utilBelugaDir(void);

#endif
