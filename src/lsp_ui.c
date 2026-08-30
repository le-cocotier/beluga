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

// ─── ANSI helpers ─────────────────────────────────────────────────────────────

// Move terminal cursor to absolute (col, row) — 1-based
static void ab_move(struct abuf *ab, int row, int col) {
    char esc[32];
    int  n = snprintf(esc, sizeof(esc), "\x1b[%d;%dH", row, col);
    abAppend(ab, esc, n);   // your existing abAppend
}

static void ab_sgr(struct abuf *ab, const char *codes) {
    char esc[32];
    int  n = snprintf(esc, sizeof(esc), "\x1b[%sm", codes);
    abAppend(ab, esc, n);
}

#define AB_RESET(ab)      ab_sgr(ab, "0")
#define AB_BOLD(ab)       ab_sgr(ab, "1")
#define AB_DIM(ab)        ab_sgr(ab, "2")
#define AB_FG(ab, c)      ab_sgr(ab, c)   // e.g. "32" = green
#define AB_BG(ab, c)      ab_sgr(ab, c)   // e.g. "44" = blue bg

// ─── Completion item kind → short tag ────────────────────────────────────────

// Tag/kind numbers are the LSP CompletionItemKind enum values (1-indexed).
static const char *kind_tag(int k) {
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

static const char *kind_color(int k) {
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
//  ┌──────────────────────┐
//  │ include <header>     │  ← selected (reverse video)
//  │ include "header"     │
//  │ include_next "header │
//  │ include_next <header │
//  │ define macro(args)   │
//  │ define macro         │
//  │ error message        │
//  │ if condition         │
//  └──────────────────────┘
//
// Drawn using absolute cursor positioning — safe to call inside
// editorRefreshScreen() before the final write().

void lspUiDrawCompletion(struct abuf *ab, CompletionPopup *popup) {
    if (!popup->visible || popup->count == 0) return;

    int visible_count = popup->count < COMPLETION_POPUP_H
                      ? popup->count : COMPLETION_POPUP_H;

    // Scroll window so selected item is always visible
    int scroll = 0;
    if (popup->selected >= COMPLETION_POPUP_H)
        scroll = popup->selected - COMPLETION_POPUP_H + 1;

    // Size the box to the longest visible label, capped at COMPLETION_MAX_WIDTH
    int content_w = 0;
    for (int i = 0; i < visible_count; i++) {
        int idx = i + scroll;
        int label_len = (int) strlen(popup->items[idx].label);
        if (label_len > content_w) content_w = label_len;
    }
    // +5 for "| " + a 3-char kind tag + the space separating it from the label
    if (content_w > COMPLETION_MAX_WIDTH - 9) content_w = COMPLETION_MAX_WIDTH - 9;
    int box_w = content_w + 9; // "| " + tag(3) + " " + text + " |"

    // ── top border ────────────────────────────────────────────────────────────
    ab_move(ab, popup->origin_y, popup->origin_x);
    ab_sgr(ab, "0;37;40");
    abAppend(ab, "┌", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┐", 3);

    // ── rows ──────────────────────────────────────────────────────────────────
    for (int i = 0; i < visible_count; i++) {
        int idx = i + scroll;
        CompletionItem *it = &popup->items[idx];
        int is_sel = (idx == popup->selected);
        const char *row_style = is_sel ? "7" : "0;37;40";

        ab_move(ab, popup->origin_y + 1 + i, popup->origin_x);
        ab_sgr(ab, row_style);
        abAppend(ab, "| ", 2);

        // Kind tag ("fn ", "var", "kwd"...) in its own color, then back to
        // the row's base style (reverse video for the selected row, dim
        // white otherwise) for the label text.
        ab_sgr(ab, kind_color(it->kind));
        abAppend(ab, kind_tag(it->kind), 3);
        ab_sgr(ab, row_style);
        abAppend(ab, " ", 1);

        char label_col[COMPLETION_MAX_WIDTH];
        snprintf(label_col, sizeof(label_col), "%-*s", content_w, it->label);
        // truncate if the label itself is longer than content_w
        label_col[content_w] = '\0';
        abAppend(ab, label_col, content_w);

        abAppend(ab, " |", 2);
        AB_RESET(ab);
    }

    // ── bottom border ─────────────────────────────────────────────────────────
    ab_move(ab, popup->origin_y + 1 + visible_count, popup->origin_x);
    ab_sgr(ab, "0;37;40");
    abAppend(ab, "└", 3);
    for (int i = 0; i < box_w - 2; i++) abAppend(ab, "─", 3);
    abAppend(ab, "┘", 3);
    AB_RESET(ab);
}


int lspUiHandleKey(CompletionPopup *popup, Utf8Char *key) {
    if (!popup->visible) return 0;

    unsigned char c = key->character[0];
    int is_single = (key->length == 1);

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
                                       int buffer_id, int line) {
    DiagSeverity worst = 0;
    for (int i = 0; i < diags->count; i++) {
        Diagnostic *d = &diags->entries[i];
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
                     int buf_id, int line, GitLineStatus git_status) {
    DiagSeverity s = diag_worst_on_line(diags, buf_id, line);
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
                                    int buffer_id, int cursor_line) {
    for (int i = 0; i < diags->count; i++) {
        Diagnostic *d = &diags->entries[i];
        if (d->buffer_id == buffer_id && d->line == cursor_line)
            return d->message;
    }
    return NULL;
}

// ─── Parse clangd JSON → our structs ─────────────────────────────────────────

void lspParseCompletion(const char *json, CompletionPopup *popup,
                        int screen_x, int screen_y) {
    popup->count    = 0;
    popup->selected = 0;
    popup->origin_x = screen_x + 1;
    popup->origin_y = screen_y + 2;  // one row below cursor

    cJSON *root  = cJSON_Parse(json);
    if (!root) return;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result) { cJSON_Delete(root); return; }
    // result can be a list or {isIncomplete, items:[…]}
    cJSON *items = cJSON_IsArray(result)
                 ? result
                 : cJSON_GetObjectItem(result, "items");
    if (!items) { cJSON_Delete(root); return; }

    cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (popup->count >= COMPLETION_MAX_ITEMS) break;
        CompletionItem *ci = &popup->items[popup->count++];

        cJSON *label  = cJSON_GetObjectItem(item, "label");
        cJSON *insertText = cJSON_GetObjectItem(item, "insertText");
        cJSON *detail = cJSON_GetObjectItem(item, "detail");
        cJSON *kind   = cJSON_GetObjectItem(item, "kind");

        const char *raw_label = label ? label->valuestring : "";
        if (raw_label[0] == ' ') raw_label++;

        strncpy(ci->label,  raw_label,                          127);
        strncpy(ci->detail, detail ? detail->valuestring : "",   63);

        const char *raw_insert = (insertText && insertText->valuestring)
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
                         int buffer_id) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!params) { cJSON_Delete(root); return; }
    cJSON *diag_arr = cJSON_GetObjectItem(params, "diagnostics");
    if (!diag_arr) { cJSON_Delete(root); return; }

    // Remove old entries for this buffer
    int w = 0;
    for (int i = 0; i < diags->count; i++)
        if (diags->entries[i].buffer_id != buffer_id)
            diags->entries[w++] = diags->entries[i];
    diags->count = w;

    cJSON *d;
    cJSON_ArrayForEach(d, diag_arr) {
        if (diags->count >= DIAG_MAX) break;
        Diagnostic *diag = &diags->entries[diags->count++];
        diag->buffer_id = buffer_id;

        cJSON *range    = cJSON_GetObjectItem(d, "range");
        cJSON *start    = cJSON_GetObjectItem(range, "start");
        cJSON *end_pos  = cJSON_GetObjectItem(range, "end");
        cJSON *sev      = cJSON_GetObjectItem(d, "severity");
        cJSON *msg      = cJSON_GetObjectItem(d, "message");

        diag->line      = cJSON_GetObjectItem(start, "line")->valueint;
        diag->col_start = cJSON_GetObjectItem(start, "character")->valueint;
        diag->col_end   = cJSON_GetObjectItem(end_pos, "character")->valueint;
        diag->severity  = sev ? (DiagSeverity)sev->valueint : DIAG_ERROR;

        const char *raw = msg ? msg->valuestring : "";
        int i = 0;
        while (raw[i] && raw[i] != '\n' && i < 255)
        {
            diag->message[i] = raw[i];
            i++;
        }
        diag->message[i] = '\0';
    }

    cJSON_Delete(root);
}
