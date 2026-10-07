
#include "../include/utils.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief realloc that zero-initializes newly added memory (like calloc does)
 * @param ptr       Existing allocation (or NULL for a fresh allocation)
 * @param old_size  Size in bytes of the existing allocation (0 if ptr is NULL)
 * @param new_size  Desired new size in bytes
 * @return New pointer on success, NULL on failure (original ptr untouched, per realloc semantics)
 */
void* recalloc(void* ptr, const size_t old_size, const size_t new_size)
{
    void* new_ptr = grealloc(ptr, new_size);
    if (new_ptr == NULL)
        return NULL;

    if (new_size > old_size)
        memset((char*)new_ptr + old_size, 0, new_size - old_size);

    return new_ptr;
}

char *utilStrdup(const char *s)
{
    return utilStrndup(s, strlen(s));
}

char *utilStrndup(const char *s, const size_t n)
{
    size_t len = 0;
    char *copy;
    while (len < n && s[len])
        len++;
    copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *utilStrtok(char *str, const char *delim, char **saveptr)
{
    char *start = str ? str : *saveptr;
    char *end;
    if (!start)
        return NULL;
    start += strspn(start, delim);
    if (*start == '\0')
    {
        *saveptr = start;
        return NULL;
    }
    end = start + strcspn(start, delim);
    if (*end)
        *end++ = '\0';
    *saveptr = end;
    return start;
}

void *utilMemmem(const void *haystack, const size_t haystack_len, const void *needle,
                 const size_t needle_len)
{
    const unsigned char *h = haystack;
    const unsigned char first = needle_len ? *(const unsigned char *) needle : 0;
    size_t i;
    if (needle_len == 0)
        return (void *) haystack;
    for (i = 0; i + needle_len <= haystack_len; i++)
        if (h[i] == first && memcmp(h + i, needle, needle_len) == 0)
            return (void *) (h + i);
    return NULL;
}

char *utilFormat(const char *fmt, ...)
{
    va_list ap;
    int len;
    char *out;

    va_start(ap, fmt);
    len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0)
        return NULL;
    out = malloc((size_t) len + 1);
    if (!out)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(out, (size_t) len + 1, fmt, ap);
    va_end(ap);
    return out;
}

void utilDirname(const char *path, char *out, const size_t out_size)
{
    const int drive = os_path_drive_length(path);
    size_t len = strlen(path);

    while (len > (size_t) drive + 1 && path[len - 1] == '/') // trailing slashes
        len--;
    while (len > (size_t) drive && path[len - 1] != '/')     // the last component
        len--;
    if (len == (size_t) drive) // no directory part
    {
        snprintf(out, out_size, "%.*s.", drive, path);
        return;
    }
    while (len > (size_t) drive + 1 && path[len - 1] == '/') // its separator(s)
        len--;
    snprintf(out, out_size, "%.*s", (int) len, path);
}
