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
#include "../include/terminal.h"
#include "../include/popup_ui.h"

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

        raw_label = label ? label->valuestring : "";
        if (raw_label[0] == ' ') raw_label++;

        strncpy(ci->label,  raw_label,                          127);
        strncpy(ci->detail, detail ? detail->valuestring : "",   63);

        raw_insert = (insertText && insertText->valuestring)
                                ? insertText->valuestring
                                : raw_label;
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
