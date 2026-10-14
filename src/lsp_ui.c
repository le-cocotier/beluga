//
// Created by Giorgio on 27/05/2026.
//

#include "../include/lsp_ui.h"
#include "../include/data.h"
#include <stdio.h>
#include <string.h>

// We use cJSON — drop cJSON.c + cJSON.h into src/ and include/
#include "../include/cJSON.h"
#include "../include/append_buffer.h"
#include "../include/popup_ui.h"
#include "../include/utf8.h"

extern EditorConfig E;

// ─── ANSI helpers ─────────────────────────────────────────────────────────────
// (the completion popup's chrome now lives in popup_ui.c — these remain for
// the per-cell gutter/diagnostic-severity coloring below, which draws single
// glyphs inline rather than whole popup rows)

static void ab_sgr(struct abuf *ab, const char *codes) {
    char esc[48];
    int  n = snprintf(esc, sizeof(esc), "\x1b[%sm", codes);
    if (n >= (int) sizeof(esc)) n = (int) sizeof(esc) - 1; // defensive: never over-read on truncation
    abAppend(ab, esc, n);
}

#define AB_RESET(ab)      ab_sgr(ab, "0")
#define AB_BOLD(ab)       ab_sgr(ab, "1")
#define AB_DIM(ab)        ab_sgr(ab, "2")
#define AB_FG(ab, c)      ab_sgr(ab, c)   // e.g. "32" = green
#define AB_BG(ab, c)      ab_sgr(ab, c)   // e.g. "44" = blue bg

// ─── Completion item kind → short tag ────────────────────────────────────────

// Tag/kind numbers are the LSP CompletionItemKind enum values (1-indexed).
static const char *kind_tag(const int k) {
    switch (k) {
        case  2: return "mth";  // Method
        case  3: return "fn ";  // Function
        case  4: return "ctr";  // Constructor
        case  5: return "fld";  // Field
        case  6: return "var";  // Variable
        case  7: return "cls";  // Class
        case  8: return "ifc";  // Interface
        case  9: return "mod";  // Module
        case 13: return "enm";  // Enum
        case 14: return "kwd";  // Keyword
        case 20: return "emb";  // EnumMember
        case 21: return "cst";  // Constant
        case 22: return "sct";  // Struct — very common completion kind in C
        default: return "   ";
    }
}

static const char *kind_color(const int k) {
    switch (k) {
        case  3:
        case  4: return "33";   // yellow  — functions/constructors
        case  2: return "36";   // cyan    — methods
        case  7:
        case 22: return "35";   // magenta — classes/structs
        case 14: return "34";   // blue    — keywords
        case  5:
        case  6: return "32";   // green   — fields/vars
        case 13:
        case 20: return "35";   // magenta — enums/enum members (type-like)
        case 21: return "32";   // green   — constants (value-like)
        default: return "37";   // white
    }
}

// ─── Draw completion popup ───────────────────────────────────────────────────
//
//  ╭──────────────────────╮
//  │▎fn  printf           │  ← selected: accent marker + tinted row
//  │ kwd if               │
//  │ var argc             │
//  ╰──────────────────────╯░
//   ░░░░░░░░░░░░░░░░░░░░░░░░
//
// Drawn using absolute cursor positioning — safe to call inside
// editorRefreshScreen() before the final write(). Chrome (border, tinted
// selection, drop shadow) is popup_ui.c's job; this function only knows
// about completion-specific content (kind tags + labels).

void lspUiDrawCompletion(struct abuf *ab, CompletionPopup *popup) {
    int visible_count;
    int scroll;
    int content_w;
    int i;
    int idx;
    int label_len;
    int box_w;
    PopupStyle style;
    CompletionItem *it;
    int is_sel;
    int row;
    char label_col[COMPLETION_MAX_WIDTH];
    if (!popup->visible || popup->count == 0) return;

    visible_count = popup->count < COMPLETION_POPUP_H
                      ? popup->count : COMPLETION_POPUP_H;

    // Scroll window so selected item is always visible
    scroll = 0;
    if (popup->selected >= COMPLETION_POPUP_H)
        scroll = popup->selected - COMPLETION_POPUP_H + 1;

    // Size the box to the longest visible label, capped at COMPLETION_MAX_WIDTH
    content_w = 0;
    for (i = 0; i < visible_count; i++) {
        idx = i + scroll;
        label_len = (int) strlen(popup->items[idx].label);
        if (label_len > content_w) content_w = label_len;
    }
    // +5 for the tag(3) + the space separating it from the label; the
    // marker/border/margin overhead is popupRowBegin/End's job, not ours.
    if (content_w > COMPLETION_MAX_WIDTH - 5) content_w = COMPLETION_MAX_WIDTH - 5;
    box_w = content_w + 4 + 5; // border+marker+margin*2 (4) + tag+space (5)

    popupStyleInit(&style);

    popupDrawTop(ab, &style, popup->origin_x, popup->origin_y, box_w);

    for (i = 0; i < visible_count; i++) {
        idx = i + scroll;
        it = &popup->items[idx];
        is_sel = (idx == popup->selected);
        row = popup->origin_y + 1 + i;

        popupRowBegin(ab, &style, popup->origin_x, row, is_sel);

        // Kind tag ("fn ", "var", "kwd"...) in its own fixed color — a
        // bare fg override, same trick as the selection marker — then
        // back to the row's own style for the label text.
        ab_sgr(ab, kind_color(it->kind));
        abAppend(ab, kind_tag(it->kind), 3);
        popupRowStyle(ab, &style, is_sel);
        abAppend(ab, " ", 1);

        snprintf(label_col, sizeof(label_col), "%-*s", content_w, it->label);
        // truncate if the label itself is longer than content_w
        label_col[content_w] = '\0';
        abAppend(ab, label_col, content_w);
        abAppend(ab, " ", 1);

        popupRowEnd(ab, &style);
    }

    popupDrawBottom(ab, &style, popup->origin_x, popup->origin_y + 1 + visible_count, box_w);
    popupDrawShadow(ab, &style, popup->origin_x, popup->origin_y, box_w, 2 + visible_count);
}


int lspUiHandleKey(CompletionPopup *popup, const Utf8Char *key) {
    unsigned char c;
    int is_single;
    if (!popup->visible) return 0;

    c = key->character[0];
    is_single = (key->length == 1);

    if (is_single && c == '\x1b') { // ESC only — dismiss
        popup->visible = 0;
        return 1;
    }
    if (is_single && (c == ARROW_UP || c == CTRL_KEY('p'))) {
        if (popup->selected > 0) popup->selected--;
        return 1;
    }
    if (is_single && (c == ARROW_DOWN || c == CTRL_KEY('n'))) {
        if (popup->selected < popup->count - 1) popup->selected++;
        return 1;
    }

    popup->visible = 0;
    return 0;
}

// ─── Hover / signature popup ─────────────────────────────────────────────────

// Code points in the first `len` bytes of s — the popup's column count.
static int utf8_cols(const char *s, const int len) {
    int i;
    int cols = 0;
    for (i = 0; i < len && s[i]; i++)
        if (((unsigned char) s[i] & 0xC0) != 0x80) cols++;
    return cols;
}

// Appends one cleaned-up line to the popup, collapsing runs of blank lines
// and dropping leading ones. Returns 0 once the popup is full.
static int info_push_line(LspInfoPopup *popup, const char *src, const int len) {
    char *dst;
    int i;
    int k;
    int n;
    int seq;
    int start;
    int end;
    unsigned char lead;
    if (popup->count >= LSP_INFO_MAX_LINES) return 0;

    start = 0;
    end = len;
    // Markdown headings ("### function `foo`") read fine without the hashes.
    while (start < end && src[start] == '#') start++;
    while (start < end && src[start] == ' ') start++;
    // Trailing spaces and the "\" markdown hard break.
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\\' ||
                           src[end - 1] == '\r'))
        end--;

    if (start == end && (popup->count == 0 || popup->lines[popup->count - 1][0] == '\0'))
        return 1;

    dst = popup->lines[popup->count];
    n = 0;
    i = start;
    while (i < end) {
        if (src[i] == '`') { i++; continue; }
        lead = (unsigned char) src[i];
        seq = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (i + seq > end) seq = end - i;
        // Truncate on a character boundary, never mid-sequence.
        if (n + seq > LSP_INFO_LINE_W - 1) break;
        for (k = 0; k < seq; k++)
            dst[n++] = src[i + k] == '\t' ? ' ' : src[i + k];
        i += seq;
    }
    dst[n] = '\0';
    popup->count++;
    return 1;
}

void lspUiSetInfo(LspInfoPopup *popup, const LspInfoKind kind, const char *text,
                  const int anchor_x, const int anchor_y) {
    const char *p;
    const char *eol;
    int len;
    popup->count = 0;
    popup->kind = kind;
    popup->anchor_x = anchor_x;
    popup->anchor_y = anchor_y;
    popup->hl_line = -1;
    popup->hl_start = 0;
    popup->hl_end = 0;

    p = text ? text : "";
    while (*p) {
        eol = strchr(p, '\n');
        len = eol ? (int) (eol - p) : (int) strlen(p);
        if (len >= 3 && strncmp(p, "```", 3) == 0) {
            // code fence: keep the code, drop the fence line itself
        } else if (len == 3 && strncmp(p, "---", 3) == 0) {
            if (!info_push_line(popup, "", 0)) break;
        } else if (!info_push_line(popup, p, len)) {
            break;
        }
        if (!eol) break;
        p = eol + 1;
    }
    while (popup->count > 0 && popup->lines[popup->count - 1][0] == '\0')
        popup->count--;
    popup->visible = popup->count > 0;
}

void lspUiDrawInfo(struct abuf *ab, const LspInfoPopup *popup) {
    int content_w;
    int box_w;
    int box_h;
    int x;
    int top;
    int fits_above;
    int fits_below;
    int i;
    int j;
    int cols;
    int len;
    int in_hl;
    const char *line;
    PopupStyle style;
    if (!popup->visible || popup->count == 0) return;

    content_w = 1;
    for (i = 0; i < popup->count; i++) {
        cols = utf8_cols(popup->lines[i], LSP_INFO_LINE_W);
        if (cols > content_w) content_w = cols;
    }
    if (content_w > E.screencols - 6) content_w = E.screencols - 6;
    if (content_w < 1) return;
    box_w = content_w + 5; // border+marker (2) + text + space + space+border (2)
    box_h = popup->count + 2;

    x = popup->anchor_x + 1;
    if (x + box_w - 1 > E.screencols) x = E.screencols - box_w + 1;
    if (x < 1) x = 1;

    // Signature help goes above the cursor, out of the completion popup's
    // way; hover goes below. Either flips when there's no room.
    fits_above = popup->anchor_y + 1 - box_h >= 1;
    fits_below = popup->anchor_y + 2 + box_h - 1 <= E.screenrows;
    if (popup->kind == LSP_INFO_SIGNATURE)
        top = (fits_above || !fits_below) ? popup->anchor_y + 1 - box_h : popup->anchor_y + 2;
    else
        top = (fits_below || !fits_above) ? popup->anchor_y + 2 : popup->anchor_y + 1 - box_h;
    if (top < 1) top = 1;

    popupStyleInit(&style);
    popupDrawTop(ab, &style, x, top, box_w);
    for (i = 0; i < popup->count; i++) {
        line = popup->lines[i];
        popupRowBegin(ab, &style, x, top + 1 + i, 0);

        len = (int) strlen(line);
        cols = 0;
        in_hl = 0;
        for (j = 0; j < len; j++) {
            if (((unsigned char) line[j] & 0xC0) != 0x80) {
                if (cols == content_w) break;
                cols++;
            }
            if (i == popup->hl_line && j == popup->hl_start && popup->hl_end > j) {
                ab_sgr(ab, style.accent_fg);
                AB_BOLD(ab);
                in_hl = 1;
            } else if (in_hl && j == popup->hl_end) {
                popupRowStyle(ab, &style, 0);
                in_hl = 0;
            }
            abAppend(ab, &line[j], 1);
        }
        if (in_hl) popupRowStyle(ab, &style, 0);
        for (; cols < content_w; cols++) abAppend(ab, " ", 1);
        abAppend(ab, " ", 1);

        popupRowEnd(ab, &style);
    }
    popupDrawBottom(ab, &style, x, top + box_h - 1, box_w);
    popupDrawShadow(ab, &style, x, top, box_w, box_h);
}

int lspUiInfoHandleKey(LspInfoPopup *popup, const Utf8Char *key) {
    unsigned char c;
    int is_single;
    if (!popup->visible) return 0;

    c = key->character[0];
    is_single = (key->length == 1);

    if (is_single && c == '\x1b') {
        popup->visible = 0;
        return 1;
    }
    // A signature stays up while its arguments are typed — "," re-requests
    // it to move the highlight — and goes away once the call is closed or
    // the cursor moves off somewhere else.
    if (popup->kind == LSP_INFO_SIGNATURE &&
        ((utf8IsPrintable(key) && !(is_single && c == ')')) ||
         (is_single && c == BACKSPACE)))
        return 0;

    popup->visible = 0;
    return 0;
}

static DiagSeverity diag_worst_on_line(DiagnosticList *diags,
                                       const int buffer_id, const int line) {
    DiagSeverity worst = 0;
    int i;
    Diagnostic *d;
    for (i = 0; i < diags->count; i++) {
        d = &diags->entries[i];
        if (d->buffer_id == buffer_id && d->line == line) {
            // LSP severities count up from most to least severe (1=Error
            // .. 4=Hint), so the more urgent diagnostic has the SMALLER
            // number — this used to pick the largest, which meant a line
            // with both an error and a hint would show the hint's marker.
            if (worst == 0 || d->severity < worst) worst = d->severity;
        }
    }
    return worst;
}

// Call this once per screen row, right before you draw the row text.
// ab     : your append buffer
// diags  : &E.lsp_diagnostics
// buf_id : active buffer id
// line   : file line number (0-based) for this screen row
void lspUiDrawGutter(struct abuf *ab, DiagnosticList *diags,
                     const int buf_id, const int line, const GitLineStatus git_status) {
    const DiagSeverity s = diag_worst_on_line(diags, buf_id, line);
    switch (s) {
        case DIAG_ERROR:
            ab_sgr(ab, "1;31");         // bold red
            abAppend(ab, "●", 3);
            break;
        case DIAG_WARNING:
            ab_sgr(ab, "1;33");         // bold yellow
            abAppend(ab, "◆", 3);
            break;
        case DIAG_INFO:
            ab_sgr(ab, "2;36");         // dim cyan
            abAppend(ab, "●", 3);
            break;
        case DIAG_HINT:
            ab_sgr(ab, "2;37");         // dim white
            abAppend(ab, "·", 2);       // U+00B7 is 2 bytes, not 1 — was truncating the glyph
            break;
        default:
            // No diagnostic on this line — fall back to the git diff
            // status, if any, so the two gutters can share one column
            // without needing a wider layout.
            switch (git_status) {
                case GIT_LINE_ADDED:
                    ab_sgr(ab, "32");       // green
                    abAppend(ab, "+", 1);
                    break;
                case GIT_LINE_MODIFIED:
                    ab_sgr(ab, "33");       // yellow
                    abAppend(ab, "~", 1);
                    break;
                default:
                    abAppend(ab, " ", 1);
                    break;
            }
            break;
    }
    AB_RESET(ab);
    abAppend(ab, " ", 1);  // gutter padding
}

// Returns the diagnostic message for the line the cursor is on, or NULL.
const char *lspUiDiagnosticAtCursor(DiagnosticList *diags,
                                    const int buffer_id, const int cursor_line) {
    int i;
    Diagnostic *d;
    for (i = 0; i < diags->count; i++) {
        d = &diags->entries[i];
        if (d->buffer_id == buffer_id && d->line == cursor_line)
            return d->message;
    }
    return NULL;
}

// ─── Parse clangd JSON → our structs ─────────────────────────────────────────

void lspParseCompletion(const char *json, CompletionPopup *popup,
                        const int screen_x, const int screen_y) {
    cJSON *root;
    cJSON *result;
    cJSON *items;
    cJSON *item;
    CompletionItem *ci;
    cJSON *label;
    cJSON *insertText;
    cJSON *detail;
    cJSON *kind;
    const char *raw_label;
    const char *raw_insert;
    cJSON *text_edit;
    cJSON *new_text;
    cJSON *range;
    cJSON *start;
    cJSON *end;
    cJSON *line_it;
    cJSON *scol_it;
    cJSON *ecol_it;
    popup->count    = 0;
    popup->selected = 0;
    popup->origin_x = screen_x + 1;
    popup->origin_y = screen_y + 2;  // one row below cursor

    root = cJSON_Parse(json);
    if (!root) return;
    result = cJSON_GetObjectItem(root, "result");
    if (!result) { cJSON_Delete(root); return; }
    // result can be a list or {isIncomplete, items:[…]}
    items = cJSON_IsArray(result)
                 ? result
                 : cJSON_GetObjectItem(result, "items");
    if (!items) { cJSON_Delete(root); return; }

    cJSON_ArrayForEach(item, items) {
        if (popup->count >= COMPLETION_MAX_ITEMS) break;
        ci = &popup->items[popup->count++];

        label = cJSON_GetObjectItem(item, "label");
        insertText = cJSON_GetObjectItem(item, "insertText");
        detail = cJSON_GetObjectItem(item, "detail");
        kind = cJSON_GetObjectItem(item, "kind");

        raw_label = (label && cJSON_IsString(label)) ? label->valuestring : "";
        // clangd prefixes labels with a space, or with "•" (U+2022) when
        // accepting the item would also insert an #include — neither is
        // part of the identifier.
        if (raw_label[0] == ' ') raw_label++;
        else if (strncmp(raw_label, "\xe2\x80\xa2", 3) == 0) raw_label += 3;

        memset(ci, 0, sizeof(*ci));
        strncpy(ci->label,  raw_label,                          127);
        strncpy(ci->detail, (detail && cJSON_IsString(detail)) ? detail->valuestring : "", 63);

        // textEdit is what the server actually wants inserted, and over
        // which range: a TextEdit {range,newText}, or an InsertReplaceEdit
        // {insert,replace,newText} — whose "replace" range also covers the
        // rest of the word after the cursor. Either way the range starts
        // at the beginning of what's already typed, which is what lets
        // accepting "printf" on a fuzzy "prf" replace the prefix instead
        // of guessing how much of it to skip.
        text_edit = cJSON_GetObjectItem(item, "textEdit");
        new_text = text_edit ? cJSON_GetObjectItem(text_edit, "newText") : NULL;
        range = NULL;
        if (text_edit) {
            range = cJSON_GetObjectItem(text_edit, "range");
            if (!range) range = cJSON_GetObjectItem(text_edit, "replace");
        }
        start = range ? cJSON_GetObjectItem(range, "start") : NULL;
        end = range ? cJSON_GetObjectItem(range, "end") : NULL;
        line_it = start ? cJSON_GetObjectItem(start, "line") : NULL;
        scol_it = start ? cJSON_GetObjectItem(start, "character") : NULL;
        ecol_it = end ? cJSON_GetObjectItem(end, "character") : NULL;
        if (new_text && cJSON_IsString(new_text) && line_it && scol_it && ecol_it) {
            ci->has_edit = 1;
            ci->edit_line = line_it->valueint;
            ci->edit_start_col = scol_it->valueint;
            ci->edit_end_col = ecol_it->valueint;
        }

        if (ci->has_edit)
            raw_insert = new_text->valuestring;
        else if (insertText && cJSON_IsString(insertText))
            raw_insert = insertText->valuestring;
        else
            raw_insert = raw_label;
        strncpy(ci->insertText, raw_insert, 127);

        if (!kind)
            ci->kind = 0;
        else
            ci->kind = kind->valueint;
    }

    popup->visible = (popup->count > 0);
    cJSON_Delete(root);
}

void lspParseDiagnostics(const char *json, DiagnosticList *diags,
                         const int buffer_id) {
    cJSON *root = cJSON_Parse(json);
    cJSON *params;
    cJSON *diag_arr;
    int w;
    int i;
    cJSON *d;
    Diagnostic *diag;
    cJSON *range;
    cJSON *start;
    cJSON *end_pos;
    cJSON *sev;
    cJSON *msg;
    cJSON *line_it;
    cJSON *scol_it;
    cJSON *ecol_it;
    const char *raw;
    if (!root) return;

    params = cJSON_GetObjectItem(root, "params");
    if (!params) { cJSON_Delete(root); return; }
    diag_arr = cJSON_GetObjectItem(params, "diagnostics");
    if (!diag_arr) { cJSON_Delete(root); return; }

    // Remove old entries for this buffer
    w = 0;
    for (i = 0; i < diags->count; i++)
        if (diags->entries[i].buffer_id != buffer_id)
            diags->entries[w++] = diags->entries[i];
    diags->count = w;

    cJSON_ArrayForEach(d, diag_arr) {
        if (diags->count >= DIAG_MAX) break;
        diag = &diags->entries[diags->count++];
        diag->buffer_id = buffer_id;

        range = cJSON_GetObjectItem(d, "range");
        start = cJSON_GetObjectItem(range, "start");
        end_pos = cJSON_GetObjectItem(range, "end");
        sev = cJSON_GetObjectItem(d, "severity");
        msg = cJSON_GetObjectItem(d, "message");

        // Every one of these is cJSON_GetObjectItem()'s NULL-on-missing —
        // dereferencing ->valueint straight off it (as this did) turns any
        // diagnostic missing a range, start, end or line/character field
        // into a null dereference. The severity and message lines below
        // were already written defensively; these four weren't. A server
        // is meant to always send a full range, so this is about not
        // crashing the editor over a payload we merely didn't expect.
        line_it = start ? cJSON_GetObjectItem(start, "line") : NULL;
        scol_it = start ? cJSON_GetObjectItem(start, "character") : NULL;
        ecol_it = end_pos ? cJSON_GetObjectItem(end_pos, "character") : NULL;

        diag->line      = line_it ? line_it->valueint : 0;
        diag->col_start = scol_it ? scol_it->valueint : 0;
        diag->col_end   = ecol_it ? ecol_it->valueint : diag->col_start;
        diag->severity  = sev ? (DiagSeverity)sev->valueint : DIAG_ERROR;

        raw = msg ? msg->valuestring : "";
        i = 0;
        while (raw[i] && raw[i] != '\n' && i < 255)
        {
            diag->message[i] = raw[i];
            i++;
        }
        diag->message[i] = '\0';
    }

    cJSON_Delete(root);
}
