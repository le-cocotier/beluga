//
// Created by Giorgio on 01/05/2026.
//

#ifndef BELUGA_UTF8_H
#define BELUGA_UTF8_H
#include <stdarg.h>   // va_list, used by utf8VaPrintf below
#include <stdint.h>
#include <stddef.h>

#include "data.h"

#define END_OF_STRING(c) c.character[0] = 0; \
c.length = 1;
#define END_OF_FILE(c) c.character[0] = 255; \
c.length = 1;

Utf8Char* readUtf8Char(void);
int32_t utf8ToInt32(const Utf8Char* character);
int32_t utf8StringCmp(Utf8Char* first_string, Utf8Char* second_string);
int32_t utf8StringNCmp(Utf8Char* first_string, Utf8Char* second_string, int n);

int utf8StringLen(const Utf8Char* character);
Utf8Char* utf8ReadFile(Utf8Char* file_name);
Utf8Char* stringToUtf8(unsigned char* string);
unsigned char* utf8ToString(const Utf8Char* string);
unsigned char* utf8ToNString(const Utf8Char* string, int n);
void utf8Cat(Utf8Char** dest, const Utf8Char* src);
Utf8Char *lastSlashUtf8(Utf8Char * string);
void utf8Cpy(Utf8Char * dest, Utf8Char * src);
void utf8NCpy(Utf8Char * dest, Utf8Char * src, int n);

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
