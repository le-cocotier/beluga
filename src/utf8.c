/**
 * @file utf8.c
 */

#include "../include/utf8.h"
#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/data.h"
#include "../include/os.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>


/**
 * @brief Reads `n` key bytes, waiting up to 100ms for each — the timing
 * raw mode's VTIME=1 gave read(): enough for the rest of an escape
 * sequence to arrive, short enough that a lone ESC still reads as ESC.
 * @return How many bytes were read (fewer than `n` if input stopped).
 */
static int readKeyBytes(void *buf, const int n)
{
    int got = 0;
    long r;
    while (got < n)
    {
        r = screenReadKeys((char *) buf + got, (size_t) (n - got), 100);
        if (r <= 0)
            break;
        got += (int) r;
    }
    return got;
}

/**
 * @brief Which function key (0 for F1 ... 11 for F12) sends CSI <number> ~,
 * or -1 for none.
 */
static int functionKeyFromTilde(const int number)
{
    if (number >= 11 && number <= 15) return number - 11;      // F1-F5
    if (number >= 17 && number <= 21) return number - 17 + 5;  // F6-F10
    if (number == 23 || number == 24) return number - 23 + 10; // F11, F12
    return -1;
}

/**
 * @brief Shift+F5-F12 as Terminal.app sends them: the VT220's F13-F20
 * codes (25, 26, 28, 29, 31-34), with no modifier. -1 for any other.
 */
static int shiftedFunctionKeyFromTilde(const int number)
{
    if (number == 25 || number == 26) return number - 25 + 4;  // S-F5, S-F6
    if (number == 28 || number == 29) return number - 28 + 6;  // S-F7, S-F8
    if (number >= 31 && number <= 34) return number - 31 + 8;  // S-F9-S-F12
    return -1;
}

/**
 * @fn readUtf8Char
 * @brief get an utf8 char in keyboard input.
 *
 * @note Allocate memory of the character
 *
 * @return utf8 char
 */
Utf8Char* readUtf8Char(void) {
    Utf8Char* new_char = gcalloc(1, sizeof(Utf8Char));
    char seq0;
    char m1;
    char m2;
    char seq1;
    char final;
    int is_alt;
    int is_ctrl;
    int is_shift;
    int fkey;
    int number;
    int modifier;
    int in_modifier;
    int seq_len;
    readKeyBytes(&new_char->character[0], 1);

    if (new_char->character[0]) {
        appDebug("%c\r\n", *new_char->character);
    }

    // Escape sequence handling
    if (new_char->character[0] == '\x1b')
    {

        if (readKeyBytes(&seq0, 1) != 1) {
            new_char->length = 1;
            return new_char; // lone ESC, nothing followed in time
        }

        // CSI ('[') and SS3 ('O') sequences need a second byte before
        // they mean anything; anything else following ESC directly is a
        // terminal's usual way of sending an Alt/Option-modified key.
        if (seq0 != '[' && seq0 != 'O')
        {
            if (seq0 == 'x' || seq0 == 'X')
                new_char->character[0] = ALT_X;
            else if (seq0 == 'f')
                // ESC f / ESC b — readline's forward-word / backward-word,
                // which is exactly what Terminal.app and iTerm2 send for
                // Option+Right / Option+Left with their default settings.
                new_char->character[0] = ALT_ARROW_RIGHT;
            else if (seq0 == 'b')
                new_char->character[0] = ALT_ARROW_LEFT;
            else if (seq0 == '\x1b')
            {
                // ESC ESC [ C  /  ESC ESC O C — a plain arrow sequence
                // behind a Meta prefix, the form a terminal in "Option as
                // Meta key" mode uses for Option+arrow.
                if (readKeyBytes(&m1, 1) == 1 && (m1 == '[' || m1 == 'O') &&
                    readKeyBytes(&m2, 1) == 1)
                {
                    switch (m2)
                    {
                        case 'C': new_char->character[0] = ALT_ARROW_RIGHT; break;
                        case 'D': new_char->character[0] = ALT_ARROW_LEFT;  break;
                        case 'A': new_char->character[0] = ARROW_UP;        break;
                        case 'B': new_char->character[0] = ARROW_DOWN;      break;
                        default:  new_char->character[0] = '\x1b';          break;
                    }
                }
                else
                    new_char->character[0] = '\x1b';
            }
            else
                // No general Alt+<key> encoding yet — the ones above are
                // the keys actually bound. Falling back to a bare ESC here
                // (instead of the actual character) used to silently
                // swallow every other Option-modified key: on many non-US
                // layouts (e.g. French AZERTY), typing symbols like "{",
                // "}", "[", "]" requires holding Option too, so with
                // "Option as Meta" enabled in the terminal, that keystroke
                // arrives as ESC+<char> and looked exactly like an
                // unrecognized Alt binding — the character itself is still
                // the best fallback.
                new_char->character[0] = seq0;
            new_char->length = 1;
            return new_char;
        }

        if (readKeyBytes(&seq1, 1) != 1) {
            new_char->character[0] = '\x1b';
            new_char->length = 1;
            return new_char; // incomplete sequence, treat as lone ESC
        }

        if (seq0 == '[')
        {
            if (seq1 >= '0' && seq1 <= '9')
            {
                // CSI <number> [; <modifier>] <final>: \x1b[3~ (Delete),
                // \x1b[15~ (F5), \x1b[3;5~ (Ctrl+Delete), \x1b[1;3C
                // (Alt+Right). Read to the final byte however long the
                // sequence is: stopping early (as this used to, after one
                // digit and one more byte) left the rest in the input to be
                // typed as text — Ctrl+Delete inserted "5~", F5 a "~".
                // modifier is xterm's 1 + (1 Shift, 2 Alt, 4 Ctrl, 8 Meta).
                number = seq1 - '0';
                modifier = 1;
                in_modifier = 0;
                seq_len = 0;
                for (;;)
                {
                    if (readKeyBytes(&final, 1) != 1 || ++seq_len > 16)
                    {
                        final = 0; // truncated or runaway: give up on it
                        break;
                    }
                    if (final >= '0' && final <= '9')
                    {
                        if (in_modifier)
                            modifier = modifier * 10 + (final - '0');
                        else
                            number = number * 10 + (final - '0');
                    }
                    else if (final == ';' && !in_modifier)
                    {
                        in_modifier = 1;
                        modifier = 0;
                    }
                    else
                        break;
                }
                is_alt = (modifier == 3 || modifier == 9);
                is_ctrl = (modifier == 5);
                is_shift = (modifier == 2);

                if (final == '~')
                {
                    switch (number)
                    {
                        case 1: case 7: new_char->character[0] = BEG_LINE;  break;
                        case 3: new_char->character[0] = is_ctrl ? CTRL_DEL_KEY : DEL_KEY; break;
                        case 4: case 8: new_char->character[0] = END_LINE;  break;
                        case 5: new_char->character[0] = PAGE_UP;           break;
                        case 6: new_char->character[0] = PAGE_DOWN;         break;
                        default:
                            // F1-F12: 11-15, 17-21, 23, 24 (the gaps are
                            // history); Shift+F5-F12 on Terminal.app: 25-34;
                            // Insert (2) and the rest are unbound.
                            fkey = functionKeyFromTilde(number);
                            if (fkey < 0 && (fkey = shiftedFunctionKeyFromTilde(number)) >= 0)
                                is_shift = 1;
                            if (fkey >= 0)
                                new_char->character[0] = (unsigned char)
                                    ((is_shift ? SHIFT_FUNCTION_KEY : FUNCTION_KEY) + fkey);
                            else
                                new_char->character[0] = UNKNOWN_KEY;
                            break;
                    }
                }
                else
                {
                    // Shift/Ctrl on these aren't bound, so they fall
                    // through to the plain key.
                    switch (final)
                    {
                        case 'C': new_char->character[0] = is_alt ? ALT_ARROW_RIGHT : ARROW_RIGHT; break;
                        case 'D': new_char->character[0] = is_alt ? ALT_ARROW_LEFT  : ARROW_LEFT;  break;
                        case 'A': new_char->character[0] = ARROW_UP;   break;
                        case 'B': new_char->character[0] = ARROW_DOWN; break;
                        case 'H': new_char->character[0] = BEG_LINE;   break;
                        case 'F': new_char->character[0] = END_LINE;   break;
                        // \x1b[1;2P: xterm's Shift+F1-F4.
                        case 'P': case 'Q': case 'R': case 'S':
                            new_char->character[0] = (unsigned char)
                                ((is_shift ? SHIFT_FUNCTION_KEY : FUNCTION_KEY) + (final - 'P'));
                            break;
                        default:  new_char->character[0] = UNKNOWN_KEY; break;
                    }
                }
            }
            else
            {
                switch (seq1)
                {
                    case 'A': new_char->character[0] = ARROW_UP;    break;
                    case 'B': new_char->character[0] = ARROW_DOWN;  break;
                    case 'C': new_char->character[0] = ARROW_RIGHT; break;
                    case 'D': new_char->character[0] = ARROW_LEFT;  break;
                    case 'H': new_char->character[0] = BEG_LINE;    break;
                    case 'F': new_char->character[0] = END_LINE;    break;
                    default:  new_char->character[0] = '\x1b';      break;
                }
            }
        }
        else // seq0 == 'O'
        {
            switch (seq1)
            {
                case 'H': new_char->character[0] = BEG_LINE; break;
                case 'F': new_char->character[0] = END_LINE; break;
                // \x1bOP-\x1bOS: F1-F4 on xterm and most terminals.
                case 'P': case 'Q': case 'R': case 'S':
                    new_char->character[0] = (unsigned char)(FUNCTION_KEY + (seq1 - 'P'));
                    break;
                default:  new_char->character[0] = '\x1b';   break;
            }
        }

        new_char->length = 1;
        return new_char;
    }

    // Calculating the length of the character (unchanged, for real UTF-8 input)
    if (new_char->character[0] < 0x80)
        new_char->length = 1;
    else if (new_char->character[0] < 0xE0)
        new_char->length = 2;
    else if (new_char->character[0] < 0xF0)
        new_char->length = 3;
    else
        new_char->length = 4;

    if (new_char->length > 1)
        readKeyBytes(&new_char->character[1], new_char->length - 1);

    return new_char;
}

/**
 * @brief Compares two characters: by their bytes (which, for UTF-8, orders
 * them by codepoint), then by length.
 */
static int utf8CharCmp(const Utf8Char* a, const Utf8Char* b)
{
    const int n = a->length < b->length ? a->length : b->length;
    const int d = memcmp(a->character, b->character, (size_t) n);
    return d ? d : a->length - b->length;
}

/**
 * @brief Compares two Utf8Char strings.
 * @return 0 if equal, otherwise <0 / >0 like strcmp().
 * @note Replaces a version that, on a byte-length mismatch (e.g. "é" vs
 * "e"), stopped without advancing and returned the difference of the
 * previous — equal — characters: "café.c" and "cafe.c" compared equal, and
 * a mismatch on the very first character read before the start of the
 * array.
 */
int32_t utf8StringCmp(Utf8Char* first_string, Utf8Char* second_string)
{
    int d;
    for (;; first_string++, second_string++)
    {
        d = utf8CharCmp(first_string, second_string);
        if (d || first_string->character[0] == '\0')
            return d;
    }
}

/**
 * @brief Like utf8StringCmp(), but compares at most `n` characters.
 */
int32_t utf8StringNCmp(Utf8Char* first_string, Utf8Char* second_string, int n)
{
    int d;
    for (; n > 0; n--, first_string++, second_string++)
    {
        d = utf8CharCmp(first_string, second_string);
        if (d || first_string->character[0] == '\0')
            return d;
    }
    return 0;
}

int utf8StringLen(const Utf8Char* character)
{
    int i = 0;
    const Utf8Char* pFirst = (Utf8Char *) character;
    while (*pFirst->character)
    {
        pFirst++;
        ++i;
    }
    return ++i;
}

/**
 * @brief Consume string to fill the Utf8Char struct
 * @param string String to find char to create
 * @param new_char Already allocated character to store data
 * @return return the amount of char consumed
 */
int utf8CreateChar(const unsigned char* string, Utf8Char* new_char)
{
    int i;
    // Callers may hand us galloc'd (uninitialized) storage; keep the bytes
    // past `length` deterministic for anything that copies or compares the
    // whole 4-byte array.
    memset(new_char->character, 0, sizeof(new_char->character));
    new_char->character[0] = string[0];
    // Calculating the length of the character
    if (new_char->character[0] < 0x80) // only one
    {
        new_char->length = 1;
    }
    else if (new_char->character[0] < 0xE0) // 2
    {
        new_char->length = 2;
    }
    else if (new_char->character[0] < 0xF0) // 3
    {
        new_char->length = 3;
    }
    else // 4
    {
        new_char->length = 4;
    }

    if (new_char->length > 1 ) // Getting all the remaining bytes of the character
    {
        // Invalid UTF-8 (a stray continuation byte, or a sequence cut short
        // — by the string's terminator too): the lead byte stands alone.
        // Reading the full length regardless ran past the end of the
        // string, and swallowed the bytes that follow.
        if (new_char->character[0] < 0xC0)
            new_char->length = 1;
        for (i = 1; i < new_char->length; i++)
        {
            if ((string[i] & 0xC0) != 0x80)
            {
                new_char->length = 1;
                break;
            }
        }
        if (new_char->length > 1)
            memcpy(new_char->character + 1, string + 1, new_char->length - 1);
    }

    return new_char->length;
}

/**
 *
 * @param string utf8 string to convert
 * @return An allocated string (char*)
 */
unsigned char* utf8ToString(const Utf8Char* string)
{
    unsigned char* new_string = gcalloc(1, sizeof(unsigned char));
    int i = 0;
    while (*string->character != '\0')
    {
        new_string = (unsigned char*) grealloc(new_string, (i + string->length + 2) * sizeof(unsigned char));
        memcpy(new_string + i, string->character, string->length);
        i += string->length;
        string++;
    }
    new_string[i] = '\0';
    return new_string;

}


unsigned char* utf8ToNString(const Utf8Char* string, int n) {
    unsigned char* new_string = gcalloc( (n * 4) + 1, sizeof(unsigned char));
    // Allocation is in most case. It's done to optimize memory access. One allocation for all string.
    int i = 0;
    while (*string->character != '\0' && n--)
    {
        memcpy(new_string + i, string->character, string->length);
        i += string->length;
        string++;
    }
    new_string[i] = '\0';
    return new_string;

}

/**
 *
 * @param string a char string to convert in utf8
 * @return An allocated utf8 string
 */
Utf8Char* stringToUtf8(unsigned char* string)
{
    // +1 for the END_OF_STRING terminator written after the loop.
    Utf8Char* new_string = gcalloc(strlen((char*)string) + 1, sizeof(Utf8Char));
    int i = 0;
    while (*string != '\0')
    {
        string += utf8CreateChar(string, new_string + i);
        ++i;
    }
    END_OF_STRING(new_string[i]);
    return new_string;
}

Utf8Char* utf8ReadFile(const Utf8Char* file_name)
{
    char* path = (char*) utf8ToString(file_name);
    FILE* file = fopen(path, "rb");
    size_t cap;
    size_t len;
    size_t n;
    unsigned char* bytes;
    unsigned char* grown;
    Utf8Char* file_data;
    size_t count;
    size_t i;
    gfree(path);
    if (file == NULL)
    {
        // File doesn't exist (or isn't readable): not fatal, the caller
        // treats this as "open a new file" and starts with an empty buffer.
        appDebug("error opening file");
        return NULL;
    }

    // Slurp the whole file, then decode it in one pass into an array sized
    // up front (a file never has more characters than bytes). This used to
    // go line by line through getline() and grow the result by a single
    // Utf8Char per character — 5M reallocs for a 5MB file. The 4 zeroed
    // padding bytes let utf8CreateChar() read a multi-byte sequence
    // truncated at EOF without running past the buffer.
    cap = 1 << 16;
    len = 0;
    bytes = galloc(cap + 4);
    if (bytes == NULL)
        die("utf8ReadFile");
    while ((n = fread(bytes + len, 1, cap - len, file)) > 0)
    {
        len += n;
        if (len == cap)
        {
            cap *= 2;
            grown = grealloc(bytes, cap + 4);
            if (grown == NULL)
                die("utf8ReadFile");
            bytes = grown;
        }
    }
    fclose(file);
    memset(bytes + len, 0, 4);
    appDebug("[read file] %zu bytes", len);

    file_data = galloc((len + 1) * sizeof(Utf8Char));
    if (file_data == NULL)
        die("utf8ReadFile");
    count = 0;
    for (i = 0; i < len; count++)
        i += (size_t) utf8CreateChar(bytes + i, &file_data[count]);
    gfree(bytes);
    END_OF_STRING(file_data[count]);

    return file_data;
}

/**
 * @brief concat the src in the dest
 * @param dest second utf8 string
 * @param src first utf8 string
 */
void utf8Cat(Utf8Char** dest, const Utf8Char* src)
{
    Utf8Char* temp = NULL;
    temp = (Utf8Char*) grealloc(*dest, (utf8StringLen(src) + utf8StringLen(*dest) - 1) * sizeof(Utf8Char));
    if (temp == NULL)
        return;
    *dest = temp;
    memcpy(*dest + utf8StringLen(*dest) - 1, src, utf8StringLen(src) * sizeof(Utf8Char));
}

/**
 * @brief Return a pointer to the last slash of the given string or NULL if no slash is found
 */
const Utf8Char *lastSlashUtf8(const Utf8Char * string){
    const Utf8Char * last_slash = NULL;
    while (*string->character) { //Look at the entire string
        if (*string->character == '/')
            last_slash = string;
        string++;
    }
    return last_slash;
}

int utf8FilenameExtension(const Utf8Char *filename, char *out, const size_t out_size)
{
    int len;
    int dot;
    int i;
    size_t j;
    out[0] = '\0';
    if (!filename || out_size == 0)
        return 0;

    len = utf8StringLen(filename); // includes the terminator
    if (len < 3) return 0; // need at least "x.e" -> 3 real chars + terminator

    dot = -1;
    for (i = 0; i < len - 1; i++)
        if (filename[i].length == 1 && filename[i].character[0] == '.')
            dot = i;
    if (dot < 0) return 0;

    j = 0;
    for (i = dot + 1; i < len - 1 && j < out_size - 1; i++)
    {
        if (filename[i].length != 1) return 0; // non-ASCII: no registered extension can match
        out[j++] = (char) filename[i].character[0];
    }
    out[j] = '\0';
    return j > 0;
}

void utf8Cpy(Utf8Char * dest, const Utf8Char * src) {
    int i;
    while (*src->character != '\0') {
        dest->length = src->length;
        for (i = 0; i < dest->length; ++i)
            dest->character[i] = src->character[i];
        src++;
        dest++;
    }
    END_OF_STRING((*dest));
}

void utf8NCpy(Utf8Char * dest, const Utf8Char * src, int n) {
    int i;
    while (*src->character != '\0' && n > 0) {
        dest->length = src->length;
        for (i = 0; i < dest->length; ++i)
            dest->character[i] = src->character[i];
        src++;
        dest++;
        n--;
    }
    END_OF_STRING((*dest));
}

/**
 * @brief Print a formatted string into the destination Utf8Char buffer. the format can be %s, %d, %u (for utf8 strings)
 * @param dest The destination Utf8Char buffer
 * @param capacity The number of Utf8Char elements available at dest (including room for the terminator)
 * @param format The format string
 * @param ... The arguments to format
 * @return The number of characters printed
 */
int utf8Printf(Utf8Char * dest, const size_t capacity, const char * format, ...) {
    int format_len = 0;
    int written_len = 0;
    size_t used = 0;
    Utf8Char* string = NULL;
    char temp[40] = {0};
    //analyse the string for %s, %d, %u
    va_list ap;
    size_t remaining;
    Utf8Char* u_arg;
    va_start(ap, format);

    if (capacity == 0) {
        va_end(ap);
        return 0;
    }

    while (*format && used + 1 < capacity) {
        remaining = capacity - 1 - used;
        if (*format == '%') {
            format++;
            switch (*format) {
                case 's':
                    string = stringToUtf8((unsigned char *) va_arg(ap, char *));
                    written_len = utf8StringLen(string) - 1;
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    gfree(string);
                    string = NULL;
                    break;
                case 'd':
                    written_len = sprintf(temp,"%d", va_arg(ap, int));
                    string = stringToUtf8((unsigned char *) temp);
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    gfree(string);
                    string = NULL;
                    break;
                case 'u':
                    {
                        u_arg = va_arg(ap, Utf8Char*);
                        written_len = utf8StringLen(u_arg) - 1; // see Bug 2 for the -1
                        if ((size_t) written_len > remaining)
                            written_len = (int) remaining;
                        utf8NCpy(dest, u_arg, written_len);
                    }
                    break;
                default:
                    // Unrecognized specifier (a typo'd %x, a stray %%, ...):
                    // write nothing rather than reuse whatever written_len
                    // was left over from the previous substitution — that
                    // used to silently replay the last write's length here.
                    written_len = 0;
                    break;
            }
            dest += written_len;
            used += written_len;
            format_len += written_len;
            format++;
        } else {
            format += utf8CreateChar((const unsigned char *) format, dest);
            dest++;
            ++used;
            format_len++;
        }
    }
    va_end(ap);
    END_OF_STRING((*dest));

    return format_len;
}

/**
 * @brief Print a formatted string into the destination Utf8Char buffer. the format can be %s, %d, %u (for utf8 strings)
 * @param dest The destination Utf8Char buffer
 * @param capacity The number of Utf8Char elements available at dest (including room for the terminator)
 * @param format The format string
 * @param ap The arguments to format, as a va_list
 * @return The number of characters printed
 */
int utf8VaPrintf(Utf8Char * dest, size_t capacity, const char * format, va_list ap) {
    int format_len = 0;
    int written_len = 0;
    size_t used = 0;
    Utf8Char* string = NULL;
    char temp[40] = {0};
    size_t remaining;
    Utf8Char* u_arg;
    //analyse the string for %s, %d, %u

    if (capacity == 0)
        return 0;

    while (*format && used + 1 < capacity) {
        remaining = capacity - 1 - used;
        if (*format == '%') {
            format++;
            switch (*format) {
                case 's':
                    string = stringToUtf8((unsigned char *) va_arg(ap, char *));
                    written_len = utf8StringLen(string) - 1;
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    gfree(string);
                    string = NULL;
                    break;
                case 'd':
                    written_len = sprintf(temp,"%d", va_arg(ap, int));
                    string = stringToUtf8((unsigned char *) temp);
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    gfree(string);
                    string = NULL;
                    break;
                case 'u':
                    {
                        u_arg = va_arg(ap, Utf8Char*);
                        written_len = utf8StringLen(u_arg) - 1;
                        if ((size_t) written_len > remaining)
                            written_len = (int) remaining;
                        utf8NCpy(dest, u_arg, written_len);
                    }
                    break;
                default:
                    // Unrecognized specifier (a typo'd %x, a stray %%, ...):
                    // write nothing rather than reuse whatever written_len
                    // was left over from the previous substitution — that
                    // used to silently replay the last write's length here.
                    written_len = 0;
                    break;
            }
            dest += written_len;
            used += written_len;
            format_len += written_len;
            format++;
        } else {
            format += utf8CreateChar((const unsigned char*) format, dest);
            dest++;
            ++used;
            format_len++;
        }
    }
    END_OF_STRING((*dest));
    return format_len;
}

/**
 * @brief Determine whether a Utf8Char represents a printable, insertable
 * character — as opposed to a control code, a synthetic key sentinel
 * (arrows, page up/down, etc.), or a malformed/empty character.
 * @param c The character to check
 * @return 1 if printable, 0 otherwise
 */
int utf8IsPrintable(const Utf8Char* c)
{
    unsigned char ch;
    if (c == NULL || c->length < 1 || c->length > 4)
        return 0;

    if (c->length == 1)
    {
        ch = c->character[0];

        // ASCII control characters (0x00-0x1F) and DEL (0x7F) are never printable
        if (ch < 0x20 || ch == 0x7F)
            return 0;

        // Synthetic sentinel range reserved for ARROW_UP/PAGE_DOWN/F5/etc
        // (0x80-0xBF) — see define.h. These are never real printable text,
        // even though they're single-byte and >= 0x20.
        if (ch >= 0x80 && ch <= 0xBF)
            return 0;

        return 1; // plain printable ASCII
    }

    // Multi-byte (length 2-4): genuine UTF-8 sequences are printable text.
    // (Real terminal-emulation edge cases — combining marks, zero-width
    // joiners, etc. — aren't screened out here; add codepoint-range checks
    // via utf8CharCodepoint() if you need to exclude those later.)
    return 1;
}

/**
 * @brief Decode the Unicode codepoint stored in a Utf8Char.
 * @note Utf8Char already carries its own byte length, so this is a direct
 * decode of the 1-4 raw bytes — no lookahead into a byte stream needed.
 * @return the codepoint, or U+FFFD for a malformed length
 */
uint32_t utf8CharCodepoint(const Utf8Char *c)
{
    const unsigned char *b = c->character;
    switch (c->length)
    {
    case 1: return b[0];
    case 2: return ((uint32_t)(b[0] & 0x1F) << 6) | (b[1] & 0x3F);
    case 3: return ((uint32_t)(b[0] & 0x0F) << 12) | ((uint32_t)(b[1] & 0x3F) << 6) | (b[2] & 0x3F);
    case 4: return ((uint32_t)(b[0] & 0x07) << 18) | ((uint32_t)(b[1] & 0x3F) << 12) |
                   ((uint32_t)(b[2] & 0x3F) << 6) | (b[3] & 0x3F);
    default: return 0xFFFD;
    }
}

typedef struct { uint32_t first; uint32_t last; } CodepointRange;

static int inRanges(const uint32_t cp, const CodepointRange *ranges, const int n)
{
    int lo = 0;
    int hi = n - 1;
    int mid;
    if (cp < ranges[0].first || cp > ranges[n - 1].last)
        return 0;
    while (lo <= hi)
    {
        mid = (lo + hi) / 2;
        if (cp > ranges[mid].last) lo = mid + 1;
        else if (cp < ranges[mid].first) hi = mid - 1;
        else return 1;
    }
    return 0;
}

// Characters that take no column of their own: combining marks, which
// draw over the previous character, and invisible format characters.
static const CodepointRange zero_width[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0902}, {0x093A, 0x093A},
    {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957},
    {0x0962, 0x0963}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E},
    {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0x302A, 0x302D}, {0x3099, 0x309A},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF},
    {0xE0001, 0xE0001}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
};

// East Asian Wide and Fullwidth characters, and emoji drawn as pictures:
// two columns in every common terminal.
static const CodepointRange double_width[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F3FA},
    {0x1F400, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD},
};

int utf8CharWidth(const Utf8Char *c)
{
    const uint32_t cp = utf8CharCodepoint(c);
    if (cp < 0x300)
        return 1; // ASCII and Latin: the common case, no table lookup
    if (inRanges(cp, zero_width, (int)(sizeof(zero_width) / sizeof(zero_width[0]))))
        return 0;
    if (inRanges(cp, double_width, (int)(sizeof(double_width) / sizeof(double_width[0]))))
        return 2;
    return 1;
}

int utf8ColumnOf(const Utf8Char *chars, const int size, const int index)
{
    int col = 0;
    int i;
    for (i = 0; i < index && i < size; i++)
        col += utf8CharWidth(&chars[i]);
    if (index > size)
        col += index - size; // past the end: one column per position
    return col;
}

int utf8IndexAtColumn(const Utf8Char *chars, const int size, const int column)
{
    int col = 0;
    int i;
    int w;
    for (i = 0; i < size; i++)
    {
        w = utf8CharWidth(&chars[i]);
        if (col + w > column && w > 0)
            return i;
        col += w;
    }
    return size;
}

void utf8VisibleSpan(const Utf8Char *chars, const int size, const int x_offset, const int width,
                     Utf8Span *span)
{
    int col;
    int i;
    int w;
    memset(span, 0, sizeof(*span));
    span->start = utf8IndexAtColumn(chars, size, x_offset);
    col = utf8ColumnOf(chars, size, span->start);
    if (span->start < size && col < x_offset)
    {
        // A wide character cut by the left edge: its visible half is a blank.
        span->lead_pad = col + utf8CharWidth(&chars[span->start]) - x_offset;
        span->start++;
        col = x_offset + span->lead_pad;
    }
    // Combining marks of a character that scrolled out of view have
    // nothing left to draw on.
    while (span->start < size && utf8CharWidth(&chars[span->start]) == 0)
        span->start++;
    span->columns = span->lead_pad;
    for (i = span->start; i < size; i++)
    {
        w = utf8CharWidth(&chars[i]);
        if (span->columns + w > width)
            break;
        span->columns += w;
    }
    span->end = i;
    (void)col;
}
