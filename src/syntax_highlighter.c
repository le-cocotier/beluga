#include "../include/syntax_highlighter.h"
#include "../include/data.h"
#include "../include/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

const char *c_keywords[] = {
    "if",     "else",     "while",    "for",     "do",       "switch",
    "case",   "break",    "#include", "#define", "#if", "#endif", "#ifndef", "continue", "return",
    "goto",   "struct",   "union",    "enum",    "typedef",  "static",
    "extern", "const",    "volatile", "sizeof",  "auto",     "register",
    "inline", "restrict", NULL};

const char *c_types[] = {"int",    "char", "float", "double",
                         "void",   "long", "short", "unsigned",
                         "signed", "bool", NULL};

/**
 * @brief Decode the Unicode codepoint stored in a Utf8Char.
 * @note Utf8Char already carries its own byte length, so this is a direct
 * decode of the 1-4 raw bytes — no lookahead into a byte stream needed.
 */
static uint32_t utf8CharCodepoint(const Utf8Char *c)
{
    const unsigned char *b = c->character;
    switch (c->length)
    {
        case 1:
            return b[0];
        case 2:
            return ((uint32_t)(b[0] & 0x1F) << 6) | (b[1] & 0x3F);
        case 3:
            return ((uint32_t)(b[0] & 0x0F) << 12) |
                   ((uint32_t)(b[1] & 0x3F) << 6) | (b[2] & 0x3F);
        case 4:
            return ((uint32_t)(b[0] & 0x07) << 18) |
                   ((uint32_t)(b[1] & 0x3F) << 12) |
                   ((uint32_t)(b[2] & 0x3F) << 6) | (b[3] & 0x3F);
        default:
            return 0xFFFD; // replacement character: malformed length
    }
}

/**
 * @brief Is this character a "word" character for identifier/keyword purposes?
 * @param c A single decoded Utf8Char (not a byte pointer)
 */
int is_word_char(const Utf8Char *c)
{
    uint32_t cp = utf8CharCodepoint(c);

    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_' || cp == '#')
        return 1;

    if (cp == 0xFFFD) return 0;

    if (cp >= 0x00C0 && cp <= 0x017F) return 1;
    if (cp >= 0x0370 && cp <= 0x03FF) return 1;
    if (cp >= 0x0400 && cp <= 0x04FF) return 1;
    if (cp >= 0x0600 && cp <= 0x06FF) return 1;
    if (cp >= 0x05D0 && cp <= 0x05EA) return 1;
    if (cp >= 0x0900 && cp <= 0x097F) return 1;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
    if ((cp >= 0x3040 && cp <= 0x309F) ||
        (cp >= 0x30A0 && cp <= 0x30FF)) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 1;

    return 0;
}

/**
 * @brief True if this Utf8Char is a single-byte ASCII match for `ch`.
 * @note Guards against a multi-byte character whose first raw byte happens
 * to numerically match an ASCII value — length must also be 1.
 */
static int isAsciiChar(const Utf8Char *c, char ch)
{
    return c->length == 1 && c->character[0] == (unsigned char)ch;
}

/**
 * @brief Copy one full character (1-4 raw bytes) from a Utf8Char array into
 * a plain byte output buffer, advancing both the character index and the
 * byte-output position.
 * @details The row is still scanned character-by-character from column 0
 * regardless of horizontal scroll, since comment_section/string state must
 * see every character in order to stay correct — but only characters whose
 * column falls in [x_start, x_end) are actually written to dst, so the
 * caller can clip output to the pane's visible width without losing state.
 */
static void copy_utf8_char(char *dst, int *dst_pos, const Utf8Char *src, int *src_pos,
                           int x_start, int x_end)
{
    const Utf8Char *c = &src[*src_pos];
    if (*src_pos >= x_start && *src_pos < x_end)
    {
        memcpy(&dst[*dst_pos], c->character, c->length);
        *dst_pos += c->length;
    }
    (*src_pos)++;
}

int is_keyword(const char *word)
{
    for (int i = 0; c_keywords[i] != NULL; i++)
        if (strcmp(word, c_keywords[i]) == 0)
            return 1;
    return 0;
}

int is_type(const char *word)
{
    for (int i = 0; c_types[i] != NULL; i++)
        if (strcmp(word, c_types[i]) == 0)
            return 1;
    return 0;
}

const char *get_color(TokenType type)
{
    switch (type)
    {
        case TOKEN_KEYWORD:  return E.theme.COLOR_KEYWORD;
        case TOKEN_TYPE:     return E.theme.COLOR_TYPE;
        case TOKEN_STRING:   return E.theme.COLOR_STRING;
        case TOKEN_COMMENT:  return E.theme.COLOR_COMMENT;
        case TOKEN_NUMBER:   return E.theme.COLOR_NUMBER;
        default:             return E.theme.COLOR_DEFAULT;
    }
}

int comment_section = 0;

/**
 * @brief Highlight one editor row and return an ANSI-colored, ready-to-render
 * byte string (raw UTF-8 + color escapes — not a Utf8Char array, since this
 * is terminal output, not editable text).
 * @param row The row to highlight (row->characters / row->size)
 * @param length Out-param: length in bytes of the returned string (including terminator)
 * @return Newly allocated string, caller must free(); NULL on allocation failure
 */
char *highlight_line(const Row *row, int *length, int x_offset, int width)
{
    // Worst case per character: a color code (~20 bytes) + up to 4 raw bytes
    // + a reset code (~10 bytes). Generous margin, plus a flat buffer for
    // leading/trailing codes.
    int buf_size = (row->size + 1) * 48 + 256;
    char *result = malloc(buf_size);
    if (!result)
    {
        *length = 0;
        return NULL;
    }

    // The row is scanned in full from column 0 regardless of x_offset —
    // comment_section and string-literal state must see every character in
    // order to stay correct across the whole line (and across lines, for
    // block comments) — but copy_utf8_char only writes bytes for characters
    // whose column falls in [x_start, x_end), so the *output* is clipped to
    // exactly what the pane can show, matching how the plain-text (non-C)
    // row path already respects x_offset/width.
    int x_start = x_offset;
    int x_end = x_offset + width;

    int result_pos = 0;
    int i = 0;
    const Utf8Char *chars = row->characters;

    while (i < row->size)
    {
        if (isAsciiChar(&chars[i], ' ') || isAsciiChar(&chars[i], '\t'))
        {
            copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            continue;
        }

        if (comment_section)
        {
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_COMMENT);
            while (i < row->size)
            {
                if (isAsciiChar(&chars[i], '*') && i + 1 < row->size &&
                    isAsciiChar(&chars[i + 1], '/'))
                {
                    comment_section = 0;
                }
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            }

            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Line comments
        if (isAsciiChar(&chars[i], '/') && i + 1 < row->size &&
            isAsciiChar(&chars[i + 1], '/'))
        {
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_COMMENT);
            while (i < row->size)
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Block comments
        if (isAsciiChar(&chars[i], '/') && i + 1 < row->size &&
            isAsciiChar(&chars[i + 1], '*'))
        {
            comment_section = 1;
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_COMMENT);
            copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            while (i < row->size)
            {
                if (isAsciiChar(&chars[i], '*') && i + 1 < row->size &&
                    isAsciiChar(&chars[i + 1], '/'))
                {
                    copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                    copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                    comment_section = 0;
                    break;
                }
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            }
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Strings
        if (isAsciiChar(&chars[i], '"'))
        {
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s\"", E.theme.COLOR_STRING);
            i++;
            while (i < row->size && !isAsciiChar(&chars[i], '"'))
            {
                if (isAsciiChar(&chars[i], '\\'))
                {
                    copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                    if (i < row->size)
                        copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                }
                else
                {
                    copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                }
            }
            if (i < row->size && isAsciiChar(&chars[i], '"'))
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Character literals
        if (isAsciiChar(&chars[i], '\''))
        {
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s'", E.theme.COLOR_STRING);
            i++;
            while (i < row->size && !isAsciiChar(&chars[i], '\''))
            {
                if (isAsciiChar(&chars[i], '\\'))
                {
                    copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                    if (i < row->size)
                        copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                }
                else
                {
                    copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                }
            }
            if (i < row->size && isAsciiChar(&chars[i], '\''))
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Numbers
        if (chars[i].length == 1 && chars[i].character[0] >= '0' && chars[i].character[0] <= '9')
        {
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_NUMBER);
            while (i < row->size && (is_word_char(&chars[i]) || isAsciiChar(&chars[i], '.')))
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Identifiers and keywords
        if (is_word_char(&chars[i]))
        {
            int start = i;
            while (i < row->size && is_word_char(&chars[i]))
                i++;

            char word[256];
            int word_byte_len = 0;
            for (int w = start; w < i; w++)
            {
                const Utf8Char *wc = &chars[w];
                if (word_byte_len + wc->length >= (int) sizeof(word))
                    break; // guard against pathologically long identifiers
                memcpy(&word[word_byte_len], wc->character, wc->length);
                word_byte_len += wc->length;
            }
            word[word_byte_len] = '\0';

            TokenType type = TOKEN_DEFAULT;
            if (is_keyword(word))
                type = TOKEN_KEYWORD;
            else if (is_type(word))
                type = TOKEN_TYPE;

            // Classification above needs the full word regardless of what's
            // visible, but only the columns in [x_start, x_end) get emitted.
            // copy_utf8_char advances its index argument itself, so this
            // must be a plain while loop — pairing it with a for-loop's own
            // increment would skip every other character.
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", get_color(type));
            int w = start;
            while (w < i)
                copy_utf8_char(result, &result_pos, chars, &w, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Operators / other characters (including non-ASCII multi-byte)
        result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_DEFAULT);
        copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
        result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
    }

    result[result_pos] = '\0';
    *length = result_pos + 1;
    return result;
}
