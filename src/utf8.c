/**
 * @file utf8.c
 */

#include "../include/utf8.h"
#include "../include/terminal.h"
#include "../include/data.h"
#include "../include/utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>


/**
 * @fn readUtf8Char
 * @brief get an utf8 char in keyboard input.
 *
 * @note Allocate memory of the character
 *
 * @return utf8 char
 */
Utf8Char* readUtf8Char(void)
{
    Utf8Char* new_char = (Utf8Char*)calloc(1, sizeof(Utf8Char));
    read(STDIN_FILENO, &new_char->character[0], 1);

    if (new_char->character[0]) {
        appDebug("%c\r\n", *new_char->character);
    }

    // Escape sequence handling
    if (new_char->character[0] == '\x1b')
    {
        char seq0;

        if (read(STDIN_FILENO, &seq0, 1) != 1) {
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
            else
                // No general Alt+<key> encoding yet — only Alt-x is
                // recognized so far, matching the sole current binding.
                // Falling back to a bare ESC here (instead of the actual
                // character) used to silently swallow every other
                // Option-modified key: on many non-US layouts (e.g. French
                // AZERTY), typing symbols like "{", "}", "[", "]" requires
                // holding Option too, so with "Option as Meta" enabled in
                // the terminal, that keystroke arrives as ESC+<char> and
                // looked exactly like an unrecognized Alt binding — the
                // character itself is still the best fallback.
                new_char->character[0] = seq0;
            new_char->length = 1;
            return new_char;
        }

        char seq1;
        if (read(STDIN_FILENO, &seq1, 1) != 1) {
            new_char->character[0] = '\x1b';
            new_char->length = 1;
            return new_char; // incomplete sequence, treat as lone ESC
        }

        if (seq0 == '[')
        {
            if (seq1 >= '0' && seq1 <= '9')
            {
                // Extended sequence like \x1b[3~ (Delete), \x1b[5~ (PageUp), etc.
                char final;
                if (read(STDIN_FILENO, &final, 1) != 1 || final != '~') {
                    new_char->length = 1;
                    return new_char; // unrecognized, fall back to ESC
                }
                switch (seq1)
                {
                    case '1': new_char->character[0] = BEG_LINE;   break;
                    case '3': new_char->character[0] = DEL_KEY;    break;
                    case '4': new_char->character[0] = END_LINE;   break;
                    case '5': new_char->character[0] = PAGE_UP;    break;
                    case '6': new_char->character[0] = PAGE_DOWN;  break;
                    case '7': new_char->character[0] = BEG_LINE;   break;
                    case '8': new_char->character[0] = END_LINE;   break;
                    default:  new_char->character[0] = '\x1b';     break;
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
        read(STDIN_FILENO, &new_char->character[1], new_char->length - 1);

    return new_char;
}

/**
 * @fn uft8ToInt32
 * @brief utils function to help to compare characters
 * @param character
 *
 * @note It's stores as an int32 int \n
 * 0x01000000 | 0x00010000 | 0x00000100 | 0x00000000 \n
 * ------------------------------------------------- \n
 * byte 4     |   byte 3   |   byte 2   |   byte 1
 *
 * @return the int32 code of the char
 */
int32_t utf8ToInt32(const Utf8Char* character)
{
    int32_t code = 0;
    code += character->character[0];
    code += (character->character[1] << 2);
    code += (character->character[2] << 4);
    code += (character->character[3] << 6);

    return code;
}

/**
 *
 * @fn uft8StringCmp
 * @brief compare 2 Uft8Char strings
 *
 * @param first_string
 * @param second_string
 *
 * @return 0 if equal else difference between first_string and second_string first mismatching char.
 */
int32_t utf8StringCmp(Utf8Char* first_string, Utf8Char* second_string)
{
    Utf8Char* pFirst = first_string;
    Utf8Char* pSecond = second_string;
    unsigned char * pFirstChar = pFirst->character;
    unsigned char * pSecondChar = pSecond->character;
    int i = 0;

    do // if end of string
    {
        // check length
        if (pFirst->length != pSecond->length)
            break;
        //test char
        if (i < pFirst->length)
        {
            if (pFirstChar[i] != pSecondChar[i])
            {
                pFirst++;
                pSecond++;
                break;
            }
            ++i;
        } else
        {
            // Resetting to go to the next char
            i = 0;
            pFirst++;
            pSecond++;
            pFirstChar = pFirst->character;
            pSecondChar = pSecond->character;
        }
    } while (*pFirstChar != '\0' && *pSecondChar != '\0');
    return utf8ToInt32(pFirst - 1) - utf8ToInt32(pSecond - 1);
}

int32_t utf8StringNCmp(Utf8Char* first_string, Utf8Char* second_string, int n)
{
    Utf8Char* pFirst = first_string;
    Utf8Char* pSecond = second_string;
    unsigned char * pFirstChar = pFirst->character;
    unsigned char * pSecondChar = pSecond->character;
    int i = 0;

    do // if end of string
    {
        // check length
        if (pFirst->length != pSecond->length)
            break;
        //test char
        if (i < pFirst->length)
        {
            if (pFirstChar[i] != pSecondChar[i])
            {
                pFirst++;
                pSecond++;
                break;

            }
            ++i;
        } else
        {
            // Resetting to go to the next char
            i = 0;
            pFirst++;
            pSecond++;
            pFirstChar = pFirst->character;
            pSecondChar = pSecond->character;
            --n;
        }
    } while (*pFirstChar != '\0' && *pSecondChar != '\0' && n);
    if (n != 0)
        return n;
    return utf8ToInt32(pFirst - 1) - utf8ToInt32(pSecond - 1);
}

int utf8StringLen(const Utf8Char* string)
{
    int i = 0;
    Utf8Char* pFirst = (Utf8Char *) string;
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
    unsigned char* new_string = calloc(1, sizeof(unsigned char));
    int i = 0;
    while (*string->character != '\0')
    {
        new_string = (unsigned char*) recalloc(new_string, (i+1) * sizeof(unsigned char), (i + string->length + 2) * sizeof(unsigned char));
        memcpy(new_string + i, string->character, string->length);
        i += string->length;
        string++;
    }
    new_string[i] = '\0';
    return new_string;

}

// TODO: adapt to the right size
unsigned char* utf8ToNString(const Utf8Char* string, int n)
{
    unsigned char* new_string = calloc( n * 4, sizeof(unsigned char));
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
    Utf8Char* new_string = calloc(1, sizeof(Utf8Char));
    int i = 0;
    while (*string != '\0')
    {
        new_string = (Utf8Char*) recalloc(new_string, (i + 1) * sizeof(Utf8Char), (i + 2) * sizeof(Utf8Char));
        string += utf8CreateChar(string, new_string + i);
        ++i;
    }
    END_OF_STRING(new_string[i])
    return new_string;
}

Utf8Char* utf8ReadFile(Utf8Char* file_name)
{
    char * char_file_name = (char *) utf8ToString(file_name);
    FILE* file = fopen(char_file_name, "r");
    Utf8Char* file_data = NULL;
    Utf8Char* temp;
    int file_length = 0;
    int line_len = 0;
    int i;
    char* line = NULL;
    size_t line_cap = 0;

    if (file == NULL)
    {
        // File doesn't exist (or isn't readable): not fatal, the caller
        // treats this as "open a new file" and starts with an empty buffer.
        appDebug("error opening file");
        free(char_file_name);
        return NULL;
    }

    rewind(file);
    while ((line_len = getline(&line, &line_cap, file)) != -1) {
        i = 0;
        appDebug("[read file] %s", line);
        while (i < line_len)
        {
            temp = (Utf8Char*) recalloc(file_data, file_length * sizeof(Utf8Char), (file_length + 1) * sizeof(Utf8Char));
            if (temp == NULL)
            {
                free(file_data);
                exit(1);
            } else
            {
                file_data = temp;
                ++file_length;
            }
            i += utf8CreateChar((unsigned char*) line + i, &file_data[file_length - 1]);
        }

        line_cap = 0;  // Reset for next iteration
        free(line);
        line = NULL;
    }
    free(line);
    line = NULL;
    fclose(file);
    free(char_file_name);

    temp = recalloc(file_data, file_length * sizeof(Utf8Char), (file_length + 1) * sizeof(Utf8Char));
    ++file_length;
    if (temp == NULL)
    {
        free(file_data);
        exit(1);
    } else
    {
        file_data = temp;
    }
    END_OF_STRING(file_data[file_length - 1]);

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
    temp = (Utf8Char*) recalloc(*dest, utf8StringLen(*dest) * sizeof(Utf8Char), (utf8StringLen(src) + utf8StringLen(*dest) - 1) * sizeof(Utf8Char));
    if (temp == NULL)
        return;
    *dest = temp;
    memcpy(*dest + utf8StringLen(*dest) - 1, src, utf8StringLen(src) * sizeof(Utf8Char));
}

/**
 * @brief Return a pointer to the last slash of the given string or NULL if no slash is found
 */
Utf8Char *lastSlashUtf8(Utf8Char * string){
    Utf8Char * last_slash = NULL;
    while (*string->character) { //Look at the entire string
        if (*string->character == '/')
            last_slash = string;
        string++;
    }
    return last_slash;
}

void utf8Cpy(Utf8Char * dest, Utf8Char * src) {
    while (*src->character != '\0') {
        dest->length = src->length;
        for (int i = 0; i < dest->length; ++i)
            dest->character[i] = src->character[i];
        src++;
        dest++;
    }
    END_OF_STRING((*dest));
}

void utf8NCpy(Utf8Char * dest, Utf8Char * src, int n) {
    while (*src->character != '\0' && n > 0) {
        dest->length = src->length;
        for (int i = 0; i < dest->length; ++i)
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
int utf8Printf(Utf8Char * dest, size_t capacity, const char * format, ...) {
    int format_len = 0;
    int written_len = 0;
    size_t used = 0;
    Utf8Char* string = NULL;
    char temp[40] = {0};
    //analyse the string for %s, %d, %u
    va_list ap;
    va_start(ap, format);

    if (capacity == 0) {
        va_end(ap);
        return 0;
    }

    while (*format && used + 1 < capacity) {
        size_t remaining = capacity - 1 - used;
        if (*format == '%') {
            format++;
            switch (*format) {
                case 's':
                    string = stringToUtf8((unsigned char *) va_arg(ap, char *));
                    written_len = utf8StringLen(string) - 1;
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    free(string);
                    string = NULL;
                    break;
                case 'd':
                    written_len = sprintf(temp,"%d", va_arg(ap, int));
                    string = stringToUtf8((unsigned char *) temp);
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    free(string);
                    string = NULL;
                    break;
                case 'u':
                    {
                        Utf8Char* u_arg = va_arg(ap, Utf8Char*);
                        written_len = utf8StringLen(u_arg) - 1; // see Bug 2 for the -1
                        if ((size_t) written_len > remaining)
                            written_len = (int) remaining;
                        utf8NCpy(dest, u_arg, written_len);
                    }
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
 * @param ... The arguments to format
 * @return The number of characters printed
 */
int utf8VaPrintf(Utf8Char * dest, size_t capacity, const char * format, va_list ap) {
    int format_len = 0;
    int written_len = 0;
    size_t used = 0;
    Utf8Char* string = NULL;
    char temp[40] = {0};
    //analyse the string for %s, %d, %u

    if (capacity == 0)
        return 0;

    while (*format && used + 1 < capacity) {
        size_t remaining = capacity - 1 - used;
        if (*format == '%') {
            format++;
            switch (*format) {
                case 's':
                    string = stringToUtf8((unsigned char *) va_arg(ap, char *));
                    written_len = utf8StringLen(string) - 1;
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    free(string);
                    string = NULL;
                    break;
                case 'd':
                    written_len = sprintf(temp,"%d", va_arg(ap, int));
                    string = stringToUtf8((unsigned char *) temp);
                    if ((size_t) written_len > remaining)
                        written_len = (int) remaining;
                    utf8NCpy(dest, string, written_len);
                    free(string);
                    string = NULL;
                    break;
                case 'u':
                    {
                        Utf8Char* u_arg = va_arg(ap, Utf8Char*);
                        written_len = utf8StringLen(u_arg) - 1;
                        if ((size_t) written_len > remaining)
                            written_len = (int) remaining;
                        utf8NCpy(dest, u_arg, written_len);
                    }
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
    if (c == NULL || c->length < 1 || c->length > 4)
        return 0;

    if (c->length == 1)
    {
        unsigned char ch = c->character[0];

        // ASCII control characters (0x00-0x1F) and DEL (0x7F) are never printable
        if (ch < 0x20 || ch == 0x7F)
            return 0;

        // Synthetic sentinel range reserved for ARROW_UP/PAGE_DOWN/etc
        // (0x80-0x8F) — see define.h. These are never real printable text,
        // even though they're single-byte and >= 0x20.
        if (ch >= 0x80 && ch <= 0x8F)
            return 0;

        return 1; // plain printable ASCII
    }

    // Multi-byte (length 2-4): genuine UTF-8 sequences are printable text.
    // (Real terminal-emulation edge cases — combining marks, zero-width
    // joiners, etc. — aren't screened out here; add codepoint-range checks
    // via utf8CharCodepoint() if you need to exclude those later.)
    return 1;
}
