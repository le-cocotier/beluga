//
// Created by Giorgio on 01/05/2026.
//

#ifndef BELUGA_UTF8_H
#define BELUGA_UTF8_H
#include <stdarg.h>   // va_list, used by utf8VaPrintf below
#include <stdint.h>
#include <stddef.h>

#include "data.h"

// Turn a Utf8Char slot into the string terminator / end-of-file marker.
// These are functions (not multi-statement macros) so that they act as a
// single statement under an unbraced if/else/loop and evaluate their
// argument exactly once.
static inline void utf8SetEndOfString(Utf8Char *c)
{
    c->character[0] = 0;
    c->length = 1;
}

static inline void utf8SetEndOfFile(Utf8Char *c)
{
    c->character[0] = 255;
    c->length = 1;
}

#define END_OF_STRING(c) utf8SetEndOfString(&(c))
#define END_OF_FILE(c) utf8SetEndOfFile(&(c))

Utf8Char* readUtf8Char(void);
int32_t utf8StringCmp(Utf8Char* first_string, Utf8Char* second_string);
int32_t utf8StringNCmp(Utf8Char* first_string, Utf8Char* second_string, int n);

int utf8StringLen(const Utf8Char* character);
Utf8Char* utf8ReadFile(const Utf8Char* file_name);
Utf8Char* stringToUtf8(unsigned char* string);
unsigned char* utf8ToString(const Utf8Char* string);
unsigned char* utf8ToNString(const Utf8Char* string, int n);
void utf8Cat(Utf8Char** dest, const Utf8Char* src);
const Utf8Char *lastSlashUtf8(const Utf8Char * string);
void utf8Cpy(Utf8Char * dest, const Utf8Char * src);
void utf8NCpy(Utf8Char * dest, const Utf8Char * src, int n);

// ── Display width ────────────────────────────────────────────────────────
// Terminal columns are not characters: CJK and most emoji take 2 columns,
// combining marks and zero-width format characters take 0.

uint32_t utf8CharCodepoint(const Utf8Char *c);
// Columns one character occupies on screen: 0, 1 or 2.
int utf8CharWidth(const Utf8Char *c);
// Screen column at which chars[index] starts (index may equal size).
int utf8ColumnOf(const Utf8Char *chars, int size, int index);
// Index of the character covering `column` (size if past the end).
int utf8IndexAtColumn(const Utf8Char *chars, int size, int column);

/**
 * @brief The characters of a row that fit in `width` columns starting at
 * column `x_offset`: chars[start, end). `lead_pad` blanks come first when a
 * wide character is cut by the left edge; `columns` counts them too.
 */
typedef struct {
    int start;
    int end;
    int lead_pad;
    int columns;
} Utf8Span;
void utf8VisibleSpan(const Utf8Char *chars, int size, int x_offset, int width, Utf8Span *span);

// Extracts the part of `filename` after its last '.' into `out` (plain C
// string, NUL-terminated, truncated to fit out_size). Returns 1 on success,
// 0 if filename is NULL (a scratch buffer's fullname legitimately is),
// there's no '.', the extension is empty, or it contains a non-ASCII byte
// (no registered extension can ever match one). Shared by the LSP-server
// lookup and the syntax-highlighter language lookup — both previously kept
// their own copy of this exact scan.
int utf8FilenameExtension(const Utf8Char *filename, char *out, size_t out_size);

int utf8Printf(Utf8Char * dest, size_t capacity, const char * format, ...);
int utf8VaPrintf(Utf8Char * dest, size_t capacity, const char * format, va_list ap);

int utf8IsPrintable(const Utf8Char* c);
int utf8CreateChar(const unsigned char* string, Utf8Char* new_char);
#endif //BELUGA_UTF8_H
