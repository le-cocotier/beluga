#include "../include/syntax_highlighter.h"
#include "../include/galloc.h"
#include "../include/data.h"
#include "../include/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

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
 * @details '#' is included for every language, not just C — it costs
 * nothing for a language that never emits a bare '#' outside of a comment
 * (the comment check always runs first in highlight_line(), so it never
 * gets the chance to be scanned as a word start there), and it's what lets
 * the "c" package register "#include"/"#define"/... as ordinary multi-
 * character keyword strings rather than needing special preprocessor
 * handling of its own.
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
 * @brief Does `needle` (a short, plain-ASCII delimiter — "//", a block-open
 * like "/" + "*", "#", ...) match starting at row->characters[i]?
 * @details An empty needle always reports no match — that's how a language
 * with no block comment (say) encodes "this delimiter doesn't exist" rather
 * than every call site needing its own `lang->block_comment_open[0] &&`
 * guard before calling in.
 * @return The number of characters matched (== strlen(needle)) on a match,
 * or 0.
 */
static int rowMatchesAt(const Row *row, int i, const char *needle)
{
    if (!needle[0]) return 0;
    int n = 0;
    while (needle[n])
    {
        if (i + n >= row->size || !isAsciiChar(&row->characters[i + n], needle[n]))
            return 0;
        n++;
    }
    return n;
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

const LanguageSyntax *syntaxLanguageFor(Utf8Char *filename)
{
    char ext[16];
    if (!utf8FilenameExtension(filename, ext, sizeof(ext)))
        return NULL;

    for (int i = 0; i < E.number_of_languages; i++)
        if (strcmp(E.languages[i].extension, ext) == 0)
            return &E.languages[i];
    return NULL;
}

int comment_section = 0;

// ─── Block-comment state cache ──────────────────────────────────────────────
//
// highlight_line() relies on comment_section carrying state across
// *consecutive* calls to correctly continue a comment from one visible row
// to the next — that part is fine, and stays as-is. The gap is what
// comment_section is seeded with *before* the first visible row of a
// pane's render: previously, whatever the last call anywhere happened to
// leave it at. After a scroll or a jump (or in a second split-screen pane,
// which shares this one global with the first), that's frequently wrong —
// a block comment opened above the visible window, or in the other pane's
// buffer, was never actually seen by the highlighter, so its continuation
// rendered as plain code instead of a comment.
//
// commentStateAtRowStart() answers "does row Y start inside an unclosed
// comment?" by scanning the whole buffer from the top — output.c calls it
// once per pane, right before that pane's row loop, and seeds
// comment_section from the result.

/**
 * @brief Advances `in_comment` across one row, tracking only what's needed
 * to answer "does the *next* row start inside a comment": comment
 * continuation, line comments, block-comment openings, and skipping
 * strings/char literals whole (so a block-comment-opening sequence inside
 * one isn't mistaken for a real one) — the same token precedence
 * highlight_line() itself uses, minus everything that doesn't affect that
 * answer (numbers, identifiers, operators). Driven by `lang`'s own
 * delimiters rather than anything hardcoded, so this is exactly as correct
 * (or as inapplicable — an empty block_comment_open means it simply never
 * matches) for any registered language, not just C.
 */
static void scanRowForCommentState(const Row *row, int *in_comment, const LanguageSyntax *lang)
{
    int i = 0;
    while (i < row->size)
    {
        if (*in_comment)
        {
            int closeLen = rowMatchesAt(row, i, lang->block_comment_close);
            if (closeLen) { *in_comment = 0; i += closeLen; }
            else i++;
            continue;
        }

        // Line comment: nothing else on this row can matter once one starts.
        if (rowMatchesAt(row, i, lang->line_comment))
            return;

        int openLen = rowMatchesAt(row, i, lang->block_comment_open);
        if (openLen)
        {
            *in_comment = 1;
            i += openLen;
            continue;
        }

        int quote = -1;
        for (int q = 0; q < lang->num_quote_chars; q++)
            if (isAsciiChar(&row->characters[i], lang->quote_chars[q])) { quote = q; break; }
        if (quote >= 0)
        {
            unsigned char qc = (unsigned char) lang->quote_chars[quote];
            i++;
            while (i < row->size && !(row->characters[i].length == 1 &&
                                      row->characters[i].character[0] == qc))
            {
                if (isAsciiChar(&row->characters[i], '\\'))
                    i++; // also skip the escaped character, if there is one
                i++;
            }
            if (i < row->size)
                i++; // consume the closing quote
            continue;
        }

        i++;
    }
}

/**
 * @brief Rebuilds buf->comment_state[] in one pass over the whole buffer,
 * if it's stale (syntaxHighlighterInvalidate() was called since the last
 * build) or the row count has changed. A no-op otherwise — this is what
 * keeps re-seeding the highlighter on every frame from being an O(file
 * size) cost on every single redraw; it's only paid once per edit.
 */
static void ensureCommentStateFresh(Buffer *buf, const LanguageSyntax *lang)
{
    if (buf->comment_state_valid && buf->comment_state_numrows == buf->numrows)
        return;

    unsigned char *fresh = buf->numrows > 0 ? galloc((size_t) buf->numrows) : NULL;
    if (buf->numrows > 0 && !fresh)
        return; // best-effort: leave the previous (stale) cache in place rather than crash

    int in_comment = 0;
    for (int r = 0; r < buf->numrows; r++)
    {
        fresh[r] = (unsigned char) in_comment; // state at the *start* of this row
        scanRowForCommentState(&buf->row[r], &in_comment, lang);
    }

    gfree(buf->comment_state);
    buf->comment_state = fresh;
    buf->comment_state_numrows = buf->numrows;
    buf->comment_state_valid = 1;
}

int commentStateAtRowStart(Buffer *buf, int row)
{
    if (!buf)
        return 0;
    const LanguageSyntax *lang = syntaxLanguageFor(buf->filename);
    // No registered language, or one with no block comment at all (e.g.
    // YAML): nothing can ever be "inside" one, so there's nothing to scan.
    if (!lang || !lang->block_comment_open[0])
        return 0;
    ensureCommentStateFresh(buf, lang);
    if (!buf->comment_state || row < 0 || row >= buf->comment_state_numrows)
        return 0;
    return buf->comment_state[row];
}

void syntaxHighlighterInvalidate(Buffer *buf)
{
    if (buf)
        buf->comment_state_valid = 0;
}

void syntaxHighlighterSeed(int in_comment)
{
    comment_section = in_comment;
}

/**
 * @brief Highlight one editor row and return an ANSI-colored, ready-to-render
 * byte string (raw UTF-8 + color escapes — not a Utf8Char array, since this
 * is terminal output, not editable text).
 * @details Entirely driven by `lang`: which words are keywords vs types,
 * what starts a line/block comment, which characters open a string. There
 * is nothing language-specific left in this function itself — a package
 * registering its own language via (syntax-define-language ...) plus
 * repeated syntax-add-keyword/syntax-add-type calls gets exactly the same
 * highlighting machinery C does, because C now goes through the identical
 * path (the "c" package registers its own keyword/type list the same way).
 * @param row The row to highlight (row->characters / row->size)
 * @param length Out-param: length in bytes of the returned string (including terminator)
 * @param lang The calling buffer's registered language (must be non-NULL —
 * callers only get here once syntaxLanguageFor() has already found one)
 * @return Newly allocated string, caller must gfree(); NULL on allocation failure
 */
char *highlight_line(const Row *row, int *length, int x_offset, int width,
                     const LanguageSyntax *lang)
{
    // Worst case per character: a color code (~20 bytes) + up to 4 raw bytes
    // + a reset code (~10 bytes). Generous margin, plus a flat buffer for
    // leading/trailing codes.
    int buf_size = (row->size + 1) * 48 + 256;
    char *result = galloc(buf_size);
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
    // exactly what the pane can show, matching how the plain-text row path
    // already respects x_offset/width.
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
            // Stop coloring the moment the close delimiter is found, not at
            // end of row — a row where a multi-line comment closes partway
            // through (e.g. "  spans lines */ int x = 5;") has real code
            // after the "*/" that has to fall through to the rest of this
            // loop, not render as part of the comment. This mirrors the
            // block-comment-open branch below, which already breaks the
            // same way; a row where comment_section carries in from a
            // *previous* row used to be the one case that didn't.
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_COMMENT);
            while (i < row->size)
            {
                int closeLen = rowMatchesAt(row, i, lang->block_comment_close);
                if (closeLen)
                {
                    for (int k = 0; k < closeLen; k++)
                        copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                    comment_section = 0;
                    break;
                }
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            }

            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Line comments
        if (rowMatchesAt(row, i, lang->line_comment))
        {
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_COMMENT);
            while (i < row->size)
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Block comments
        int openLen = rowMatchesAt(row, i, lang->block_comment_open);
        if (openLen)
        {
            comment_section = 1;
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", E.theme.COLOR_COMMENT);
            for (int k = 0; k < openLen; k++)
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            while (i < row->size)
            {
                int closeLen = rowMatchesAt(row, i, lang->block_comment_close);
                if (closeLen)
                {
                    for (int k = 0; k < closeLen; k++)
                        copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
                    comment_section = 0;
                    break;
                }
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            }
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Strings — any of this language's configured quote characters,
        // all sharing the same backslash-escape handling.
        int quote_idx = -1;
        for (int q = 0; q < lang->num_quote_chars; q++)
            if (isAsciiChar(&chars[i], lang->quote_chars[q])) { quote_idx = q; break; }
        if (quote_idx >= 0)
        {
            unsigned char quote = (unsigned char) lang->quote_chars[quote_idx];
            char qbuf[2] = { (char) quote, '\0' };
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s%s", E.theme.COLOR_STRING, qbuf);
            i++;
            while (i < row->size && !(chars[i].length == 1 && chars[i].character[0] == quote))
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
            if (i < row->size && chars[i].length == 1 && chars[i].character[0] == quote)
                copy_utf8_char(result, &result_pos, chars, &i, x_start, x_end);
            result_pos += snprintf(&result[result_pos], buf_size - result_pos, "%s", COLOR_RESET);
            continue;
        }

        // Numbers — generic across every language, no per-language config
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
            for (int wi = 0; wi < lang->num_words; wi++)
            {
                if (strcmp(word, lang->words[wi].word) == 0)
                {
                    type = lang->words[wi].is_type ? TOKEN_TYPE : TOKEN_KEYWORD;
                    break;
                }
            }

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
