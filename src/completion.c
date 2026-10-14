/**
 * @file completion.c
 * @brief LSP (Language Server Protocol) client implementation for Beluga
 */

#include "../include/completion.h"
#include "../include/galloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cJSON.h"
#include "../include/data.h"
#include "../include/output.h"
#include "../include/lsp_ui.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/editor_op.h"
#include "../include/utils.h"
#include "../include/list_popup.h"
#include "../include/syntax_highlighter.h"

extern EditorConfig E;

// ─── Static Functions ──────────────────────────────────────────────────────

/**
 * @brief Monotonic clock in milliseconds, used for the completion debounce
 * timer. Immune to wall-clock adjustments, unlike time()/gettimeofday().
 */
static long monotonic_ms(void)
{
    return (long) os_monotonic_ms();
}

static void lsp_send(const LspClient *lsp, const char *json)
{
    const int body_len = (int) strlen(json);
    char header[1024];
    const int header_len = snprintf(header, sizeof(header),
                              "Content-Length: %d\r\n\r\n", body_len);

    os_proc_write(lsp->proc, header, (size_t) header_len);
    os_proc_write(lsp->proc, json, (size_t) body_len);

    appDebug("[LSP →] Content-Length: %d | %s\n", body_len, json);
}

/**
 * @brief Answers a request the *server* sent us (workspace/applyEdit, ...).
 * Takes ownership of `result` (NULL = JSON null) and of `error`; at most
 * one of the two is non-NULL.
 */
static void lsp_send_response(const LspClient *lsp, const cJSON *id, cJSON *result,
                              cJSON *error)
{
    cJSON *root;
    char *msg;
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, 1));
    if (error)
        cJSON_AddItemToObject(root, "error", error);
    else
        cJSON_AddItemToObject(root, "result", result ? result : cJSON_CreateNull());
    msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);
    gfree(msg);
    cJSON_Delete(root);
}

/**
 * @brief Pushes `buf` to the server if it changed since the last sync.
 * Position-based requests (hover, signature help, code actions) are
 * answered against the server's copy of the document, which otherwise only
 * catches up on the sync/completion debounce.
 */
static void lsp_sync_if_dirty(LspClient *lsp, struct buffer_t *buf)
{
    if (!buf->b_lsp_open || buf->dirty == buf->lsp_synced_dirty)
        return;
    lspDidChange(lsp, buf);
    buf->lsp_synced_dirty = buf->dirty;
}

/**
 * @brief The active pane's cursor, in 0-based screen coordinates — where
 * the hover/signature popup anchors. Read when the response arrives, not
 * when the request goes out: by then the pane has been redrawn, so its
 * cursor matches the buffer's.
 */
static void lsp_cursor_screen_pos(int *x, int *y)
{
    EditorPane *active = splitScreenGetActivePane();
    struct buffer_t *buf = bufferFindById(active->buffer_id);
    *x = active->cursor_x + active->origin_x + (buf ? editorGutterWidth(buf) : 0);
    *y = active->cursor_y + active->origin_y;
}

// ─── Diagnostics cache ─────────────────────────────────────────────────────
// The server's own diagnostics, verbatim, per document. A code-action
// request has to hand back the exact diagnostic objects the server
// published (clangd looks its fixes up by range + full message), which
// DiagnosticList's one-line, display-only copy can't reproduce.

typedef struct
{
    char *uri;
    cJSON *diagnostics; // array, owned
} LspDiagCache;

static LspDiagCache *g_diag_cache = NULL;
static int g_diag_cache_count = 0;

static void lsp_cache_diagnostics(const char *uri, const cJSON *diagnostics)
{
    int i;
    LspDiagCache *grown;
    for (i = 0; i < g_diag_cache_count; i++)
    {
        if (strcmp(g_diag_cache[i].uri, uri) == 0)
        {
            cJSON_Delete(g_diag_cache[i].diagnostics);
            g_diag_cache[i].diagnostics = cJSON_Duplicate(diagnostics, 1);
            return;
        }
    }
    grown = grealloc(g_diag_cache, sizeof(LspDiagCache) * (size_t) (g_diag_cache_count + 1));
    if (!grown) return;
    g_diag_cache = grown;
    g_diag_cache[g_diag_cache_count].uri = utilStrdup(uri);
    g_diag_cache[g_diag_cache_count].diagnostics = cJSON_Duplicate(diagnostics, 1);
    g_diag_cache_count++;
}

static const cJSON *lsp_cached_diagnostics(const char *uri)
{
    int i;
    for (i = 0; i < g_diag_cache_count; i++)
        if (strcmp(g_diag_cache[i].uri, uri) == 0)
            return g_diag_cache[i].diagnostics;
    return NULL;
}

static void lsp_free_diag_cache(void)
{
    int i;
    for (i = 0; i < g_diag_cache_count; i++)
    {
        gfree(g_diag_cache[i].uri);
        cJSON_Delete(g_diag_cache[i].diagnostics);
    }
    gfree(g_diag_cache);
    g_diag_cache = NULL;
    g_diag_cache_count = 0;
}

static void lsp_on_initialize_response(LspClient *lsp, const char *result_json)
{
    cJSON *init_root;
    cJSON *result;
    cJSON *caps;
    cJSON *comp_provider;
    cJSON *trig;
    cJSON *item;
    cJSON *root;
    char *msg;
    int first_buffer_id;
    struct buffer_t* b;
    // Pull completionProvider.triggerCharacters out of the server's own
    // capabilities instead of assuming word-char completion covers
    // everything — a server that only ever completes after punctuation
    // (e.g. "." for member access) would otherwise never get asked.
    lsp->num_trigger_chars = 0;
    init_root = cJSON_Parse(result_json);
    if (init_root)
    {
        result = cJSON_GetObjectItem(init_root, "result");
        caps = result ? cJSON_GetObjectItem(result, "capabilities") : NULL;
        comp_provider = caps ? cJSON_GetObjectItem(caps, "completionProvider") : NULL;
        trig = comp_provider ? cJSON_GetObjectItem(comp_provider, "triggerCharacters") : NULL;
        if (trig && cJSON_IsArray(trig))
        {
            cJSON_ArrayForEach(item, trig)
            {
                if (lsp->num_trigger_chars >= LSP_MAX_TRIGGER_CHARS) break;
                if (!cJSON_IsString(item) || !item->valuestring[0]) continue;
                strncpy(lsp->trigger_chars[lsp->num_trigger_chars],
                        item->valuestring, LSP_TRIGGER_CHAR_MAXLEN - 1);
                lsp->trigger_chars[lsp->num_trigger_chars][LSP_TRIGGER_CHAR_MAXLEN - 1] = '\0';
                lsp->num_trigger_chars++;
            }
        }
        cJSON_Delete(init_root);
    }
    if (lsp->num_trigger_chars == 0)
    {
        // Server didn't advertise any (or the response didn't parse) — "."
        // alone still covers member-access completion in most languages.
        strcpy(lsp->trigger_chars[0], ".");
        lsp->num_trigger_chars = 1;
    }

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "initialized");
    cJSON_AddItemToObject(root, "params", cJSON_CreateObject());

    msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);
    gfree(msg);
    cJSON_Delete(root);

    lsp->state = LSP_READY;
    appDebug("[LSP] handshake complete, state = READY\n");
    runHooks("lsp-ready", lisp_null());

    // Now (and only now) it's safe to open any buffers created before the
    // handshake finished — e.g. the very buffer that triggered lspStart.
    // Only buffers with a registered server: didOpen'ing everything (e.g.
    // the splash screen) sent clangd a "file" it can't compile, producing
    // a spurious diagnostic entry for a buffer the user never asked about.
    if (E.buffers != NULL)
    {
        first_buffer_id = E.buffers->buffer_id;
        b = E.buffers;
        do
        {
            // Same match as bufferMaybeSyncLsp(): a buffer opened in this
            // window (before the handshake completed) whose own
            // registered server isn't the one that just came up must not
            // be pushed to it either — a .tex buffer opened while
            // clangd's handshake was still in flight used to get
            // didOpen'd to clangd, tagged with clangd's own language_id.
            //
            // The type check has to stay *before* lspServerConfigFor():
            // only a FILE_BUFF is guaranteed a fullname, and a scratch
            // EXPLORER_BUFF (the explorer sidebar, "*search*", "*tree*")
            // has it NULL by design. Hoisting the lookup out of the &&
            // short-circuit — which is exactly what 84b881b did — walked
            // every buffer in the list and dereferenced that NULL the
            // moment a handshake completed with a sidebar open.
            if (b->type == FILE_BUFF && !b->b_lsp_open &&
                lspServerConfigMatchesRunning(lspServerConfigFor(b->fullname)))
            {
                // Same reasoning as bufferMaybeSyncLsp(): if another
                // buffer already has this exact file open and synced,
                // don't send a second didOpen for the same URI.
                if (bufferFindOtherLspOpen(b) != -1)
                    b->b_lsp_open = 1;
                else
                {
                    lspDidOpen(lsp, b);
                    b->b_lsp_open = 1;
                }
                b->lsp_synced_dirty = b->dirty;
            }
            b = b->next;
        } while (b->buffer_id != first_buffer_id);
    }
}

/**
 * @brief Register a callback to be invoked when a response with this id arrives.
 */
static void lsp_track_pending(LspClient *lsp, const int id, void (*callback)(LspClient*, const char*))
{
    if (lsp->pending_count >= LSP_MAX_PENDING)
    {
        appDebug("[LSP] pending table full, dropping tracking for id %d\n", id);
        return;
    }
    lsp->pending[lsp->pending_count].id = id;
    lsp->pending[lsp->pending_count].callback = callback;
    lsp->pending_count++;
}

/**
 * @brief Find and remove a pending entry by id; returns its callback, or NULL if not found.
 */
static void (*lsp_pop_pending(LspClient *lsp, const int id))(LspClient*, const char*)
{
    int i;
    void (*cb)(LspClient*, const char*);
    for (i = 0; i < lsp->pending_count; i++)
    {
        if (lsp->pending[i].id == id)
        {
            cb = lsp->pending[i].callback;
            // swap-remove
            lsp->pending[i] = lsp->pending[lsp->pending_count - 1];
            lsp->pending_count--;
            return cb;
        }
    }
    return NULL;
}

/**
 * @brief Feed newly-arrived bytes into the client's incremental
 * Content-Length/JSON parser. Non-blocking: only acts on complete
 * messages; partial messages persist in recv_buf across calls.
 */
static void lsp_feed(LspClient *lsp, const char *data, size_t len)
{
    // Grow the persistent receive buffer and append
    const size_t needed = lsp->recv_len + len;
    size_t new_cap;
    char *grown;
    char *header_end;
    int header_len;
    char header_copy[1024];
    int copy_len;
    int content_length;
    char *cl;
    int body_start;
    char *body;
    size_t consumed;
    if (needed > lsp->recv_cap)
    {
        new_cap = lsp->recv_cap ? lsp->recv_cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        grown = grealloc(lsp->recv_buf, new_cap);
        if (!grown) return; // best-effort: drop data rather than crash
        lsp->recv_buf = grown;
        lsp->recv_cap = new_cap;
    }
    memcpy(lsp->recv_buf + lsp->recv_len, data, len);
    lsp->recv_len += len;

    // Try to extract as many complete messages as are currently buffered
    while (1)
    {
        header_end = utilMemmem(lsp->recv_buf, lsp->recv_len, "\r\n\r\n", 4);
        if (!header_end) break; // headers not fully arrived yet

        header_len = (int)(header_end - lsp->recv_buf);
        copy_len = header_len < (int)sizeof(header_copy) - 1 ? header_len : (int)sizeof(header_copy) - 1;
        memcpy(header_copy, lsp->recv_buf, copy_len);
        header_copy[copy_len] = '\0';

        content_length = 0;
        cl = strstr(header_copy, "Content-Length: ");
        if (cl) content_length = atoi(cl + 16);

        body_start = header_len + 4;
        if (content_length <= 0 || lsp->recv_len < (size_t) body_start + (size_t) content_length)
            break; // body not fully arrived yet

        // Full message available — extract and dispatch
        body = galloc(content_length + 1);
        if (body)
        {
            memcpy(body, lsp->recv_buf + body_start, content_length);
            body[content_length] = '\0';
            appDebug("[LSP ←] %s\n", body);
            lspHandleMessage(lsp, body);
            gfree(body);
        }

        // Shift any remaining buffered bytes down to the front
        consumed = body_start + content_length;
        memmove(lsp->recv_buf, lsp->recv_buf + consumed, lsp->recv_len - consumed);
        lsp->recv_len -= consumed;
    }
}

// ─── Response handlers ─────────────────────────────────────────────────────


static void lsp_on_completion_response(LspClient *lsp, const char *result_json)
{
    // A newer completion request may have been sent (and superseded this one)
    // while this response was in flight — e.g. the user kept typing. Drop
    // anything that isn't the response to the most recently sent request so
    // a slow/out-of-order reply can never clobber a fresher, still-pending one.
    cJSON *root = cJSON_Parse(result_json);
    int id = -1;
    cJSON *id_item;
    if (root)
    {
        id_item = cJSON_GetObjectItem(root, "id");
        if (id_item && cJSON_IsNumber(id_item))
            id = id_item->valueint;
        cJSON_Delete(root);
    }
    if (id != lsp->completion_latest_request_id)
    {
        appDebug("[LSP] dropping stale completion response id=%d (latest=%d)\n",
                 id, lsp->completion_latest_request_id);
        return;
    }

    lspParseCompletion(result_json, &E.lsp_completion,
                           lsp->completion_cursor_x, lsp->completion_cursor_y);
    lsp->completion_just_arrived = 1;
    lsp->completion_requested = 0;
}

/**
 * @brief A file's URI, as sent to the server: file:// + the path — with a
 * third slash before a drive letter ("C:/dir/f.c" -> file:///C:/dir/f.c).
 */
static void lspPathToUri(const char *path, char *uri, const size_t uri_size)
{
    snprintf(uri, uri_size, "file://%s%s", path[0] == '/' ? "" : "/", path);
}

static int hexDigitValue(const char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief The local path a `file://` URI from the server names.
 * @details Percent-escapes are decoded — servers escape spaces as %20, and
 * on Windows many send the drive as "c%3A" — and on Windows the leading
 * slash before the drive is dropped and the drive letter upper-cased, to
 * match the paths realpath() gives the editor's own buffers.
 * @return 1 on success, 0 if `uri` isn't a file:// URI.
 */
static int lspUriToPath(const char *uri, char *out, const size_t out_size)
{
    const char *p;
    size_t n = 0;
    int hi;
    int lo;

    if (!uri || strncmp(uri, "file://", 7) != 0 || out_size == 0)
        return 0;
    p = uri + 7;
    while (*p && n + 1 < out_size)
    {
        hi = p[0] == '%' ? hexDigitValue(p[1]) : -1;
        lo = hi >= 0 ? hexDigitValue(p[2]) : -1;
        if (hi >= 0 && lo >= 0)
        {
            out[n++] = (char) (hi * 16 + lo);
            p += 3;
        }
        else
            out[n++] = *p++;
    }
    out[n] = '\0';
    // "/C:/dir" -> "C:/dir", with the drive letter upper-cased as the
    // editor's own paths have it (only where drive letters exist).
    if (out[0] == '/' && os_path_drive_length(out + 1) > 0)
        memmove(out, out + 1, strlen(out));
    if (os_path_drive_length(out) > 0)
        out[0] = (char) toupper((unsigned char) out[0]);
    return 1;
}

/**
 * @brief Do two paths name the same file? (Case-insensitively on Windows,
 * where file names are.)
 */
static int lspSamePath(const char *a, const char *b)
{
    return os_path_equal(a, b);
}

/**
 * @brief Maps a `file://...` URI (as sent back by the server in
 * publishDiagnostics) to the buffer_id of the matching open buffer, or -1.
 */
static int lsp_find_buffer_id_by_uri(const char *uri)
{
    int first_buffer_id;
    struct buffer_t *b;
    char path[PATH_MAX];
    char *fullname_str;
    int same;
    if (E.buffers == NULL || !lspUriToPath(uri, path, sizeof(path))) return -1;
    first_buffer_id = E.buffers->buffer_id;
    b = E.buffers;
    do
    {
        if (b->type == FILE_BUFF && b->fullname)
        {
            fullname_str = (char*) utf8ToString(b->fullname);
            same = fullname_str && lspSamePath(fullname_str, path);
            gfree(fullname_str);

            if (same)
                return b->buffer_id;
        }
        b = b->next;
    } while (b->buffer_id != first_buffer_id);
    return -1;
}

/**
 * @brief Opens (if necessary) the buffer for a `file://...` URI, switches
 * the active pane to it, and moves the cursor to (line, character) —
 * shared by go-to-definition and, indirectly, by anything else that needs
 * to land the user somewhere the server pointed at.
 * @note LSP positions are UTF-16-code-unit based per spec; like the rest
 * of this codebase's LSP integration (see lspRequestDefinition's own
 * "character" already being sent as a plain index), they're treated as
 * plain character indices here too — correct for ASCII, an approximation
 * for the rest.
 */
static void lspJumpToUri(const char *uri, const int line, const int character)
{
    int buffer_id;
    Utf8Char *path;
    struct buffer_t *buf;
    char local[PATH_MAX];
    if (!lspUriToPath(uri, local, sizeof(local)))
        return; // not a local file — nothing this editor can open

    buffer_id = lsp_find_buffer_id_by_uri(uri);
    if (buffer_id < 0)
    {
        path = stringToUtf8((unsigned char *) local);
        buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
        gfree(path);
        if (buffer_id < 0)
        {
            editorSetStatusMessage("Couldn't open %s", local);
            return;
        }
    }
    else
    {
        bufferSwitch(buffer_id);
    }

    buf = bufferFindById(buffer_id);
    if (!buf) return;

    buf->y = (line < 0) ? 0 : (line >= buf->numrows ? (buf->numrows > 0 ? buf->numrows - 1 : 0) : line);
    buf->x = (character < 0) ? 0 : character;
    if (buf->numrows > 0 && buf->x > buf->row[buf->y].size)
        buf->x = buf->row[buf->y].size;
}

static void lsp_on_definition_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *result;
    cJSON *loc;
    int jumped;
    cJSON *uri_item;
    cJSON *range;
    cJSON *start;
    cJSON *line_item;
    cJSON *char_item;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;

    result = cJSON_GetObjectItem(root, "result");
    loc = NULL;
    if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0)
        loc = cJSON_GetArrayItem(result, 0);
    else if (result && cJSON_IsObject(result))
        loc = result;

    jumped = 0;
    if (loc)
    {
        // Location has uri/range; LocationLink (what clangd actually sends)
        // has targetUri/targetRange instead.
        uri_item = cJSON_GetObjectItem(loc, "uri");
        range = cJSON_GetObjectItem(loc, "range");
        if (!uri_item) uri_item = cJSON_GetObjectItem(loc, "targetUri");
        if (!range) range = cJSON_GetObjectItem(loc, "targetRange");
        start = range ? cJSON_GetObjectItem(range, "start") : NULL;
        line_item = start ? cJSON_GetObjectItem(start, "line") : NULL;
        char_item = start ? cJSON_GetObjectItem(start, "character") : NULL;

        if (uri_item && cJSON_IsString(uri_item) && line_item && char_item)
        {
            lspJumpToUri(uri_item->valuestring, line_item->valueint, char_item->valueint);
            jumped = 1;
        }
    }
    if (!jumped)
        editorSetStatusMessage("No definition found");

    cJSON_Delete(root);
}

/**
 * @brief Extracts the display text from a hover response's
 * `result.contents`, which per spec may be a plain string, a
 * {kind,value} MarkupContent, a single MarkedString, or an array of any
 * of those — of which the first is shown.
 */
static const char *lsp_hover_contents_text(cJSON *contents)
{
    cJSON *value;
    if (!contents) return NULL;
    if (cJSON_IsString(contents)) return contents->valuestring;
    if (cJSON_IsArray(contents) && cJSON_GetArraySize(contents) > 0)
        return lsp_hover_contents_text(cJSON_GetArrayItem(contents, 0));
    if (cJSON_IsObject(contents))
    {
        value = cJSON_GetObjectItem(contents, "value");
        if (value && cJSON_IsString(value)) return value->valuestring;
    }
    return NULL;
}

static void lsp_on_hover_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *result;
    cJSON *contents;
    const char *text;
    int x;
    int y;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;

    result = cJSON_GetObjectItem(root, "result");
    contents = (result && cJSON_IsObject(result)) ? cJSON_GetObjectItem(result, "contents") : NULL;
    text = lsp_hover_contents_text(contents);

    if (text && text[0])
    {
        lsp_cursor_screen_pos(&x, &y);
        lspUiSetInfo(&E.lsp_info, LSP_INFO_HOVER, text, x, y);
    }
    if (!text || !text[0] || !E.lsp_info.visible)
        editorSetStatusMessage("No hover info");
    cJSON_Delete(root);
}

/**
 * @brief Shows result.signatures[activeSignature] above the cursor, its
 * active parameter highlighted. A null result means the cursor has left
 * the call, so an open signature popup closes.
 */
static void lsp_on_signature_help_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *result;
    cJSON *signatures;
    cJSON *sig;
    cJSON *label;
    cJSON *params;
    cJSON *param;
    cJSON *param_label;
    cJSON *doc;
    cJSON *active_item;
    const char *doc_text;
    char *text;
    size_t text_len;
    int active_sig;
    int active_param;
    int hl_start;
    int hl_end;
    const char *found;
    int x;
    int y;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;

    result = cJSON_GetObjectItem(root, "result");
    signatures = (result && cJSON_IsObject(result)) ? cJSON_GetObjectItem(result, "signatures") : NULL;
    if (!signatures || !cJSON_IsArray(signatures) || cJSON_GetArraySize(signatures) == 0)
    {
        if (E.lsp_info.kind == LSP_INFO_SIGNATURE)
            E.lsp_info.visible = 0;
        cJSON_Delete(root);
        return;
    }

    active_item = cJSON_GetObjectItem(result, "activeSignature");
    active_sig = (active_item && cJSON_IsNumber(active_item)) ? active_item->valueint : 0;
    if (active_sig < 0 || active_sig >= cJSON_GetArraySize(signatures)) active_sig = 0;
    sig = cJSON_GetArrayItem(signatures, active_sig);
    label = cJSON_GetObjectItem(sig, "label");
    if (!label || !cJSON_IsString(label))
    {
        cJSON_Delete(root);
        return;
    }

    // A signature's own activeParameter wins over the response-wide one.
    active_item = cJSON_GetObjectItem(sig, "activeParameter");
    if (!active_item) active_item = cJSON_GetObjectItem(result, "activeParameter");
    active_param = (active_item && cJSON_IsNumber(active_item)) ? active_item->valueint : -1;

    // ParameterInformation.label is either a substring of the signature
    // label or, with labelOffsetSupport, its [start, end) offsets.
    hl_start = hl_end = 0;
    params = cJSON_GetObjectItem(sig, "parameters");
    param = (params && active_param >= 0) ? cJSON_GetArrayItem(params, active_param) : NULL;
    param_label = param ? cJSON_GetObjectItem(param, "label") : NULL;
    if (param_label && cJSON_IsArray(param_label) && cJSON_GetArraySize(param_label) == 2)
    {
        hl_start = cJSON_GetArrayItem(param_label, 0)->valueint;
        hl_end = cJSON_GetArrayItem(param_label, 1)->valueint;
    }
    else if (param_label && cJSON_IsString(param_label) && param_label->valuestring[0])
    {
        found = strstr(label->valuestring, param_label->valuestring);
        if (found)
        {
            hl_start = (int) (found - label->valuestring);
            hl_end = hl_start + (int) strlen(param_label->valuestring);
        }
    }

    doc = cJSON_GetObjectItem(sig, "documentation");
    doc_text = lsp_hover_contents_text(doc);
    text_len = strlen(label->valuestring) + (doc_text ? strlen(doc_text) : 0) + 3;
    text = galloc(text_len);
    if (!text)
    {
        cJSON_Delete(root);
        return;
    }
    snprintf(text, text_len, "%s%s%s", label->valuestring,
             doc_text ? "\n\n" : "", doc_text ? doc_text : "");

    lsp_cursor_screen_pos(&x, &y);
    lspUiSetInfo(&E.lsp_info, LSP_INFO_SIGNATURE, text, x, y);
    if (E.lsp_info.count > 0 && hl_end > hl_start &&
        hl_end <= (int) strlen(E.lsp_info.lines[0]))
    {
        E.lsp_info.hl_line = 0;
        E.lsp_info.hl_start = hl_start;
        E.lsp_info.hl_end = hl_end;
    }

    gfree(text);
    cJSON_Delete(root);
}

// One flattened, sortable TextEdit — see applyTextEditsToUri().
typedef struct
{
    int start_line, start_col, end_line, end_col;
    char *new_text; // owned, freed after applying
} FlatTextEdit;

static int flatTextEditCmpDescending(const void *a, const void *b)
{
    const FlatTextEdit *ea = a, *eb = b;
    if (ea->start_line != eb->start_line) return eb->start_line - ea->start_line;
    return eb->start_col - ea->start_col;
}

/**
 * @brief Applies one file's worth of LSP TextEdit[] to the (opened if
 * necessary) buffer for `uri`. Edits are applied from the end of the
 * document backward (descending start position) so applying one never
 * shifts the coordinates an earlier-in-the-array edit was written
 * against — LSP TextEdits within one array are always relative to the
 * original, unedited document.
 */
static void applyTextEditsToUri(const char *uri, const cJSON *edits_array)
{
    int count;
    FlatTextEdit *edits;
    int n;
    int i;
    cJSON *e;
    cJSON *range;
    cJSON *new_text;
    cJSON *start;
    cJSON *end;
    int buffer_id;
    Utf8Char *path;
    struct buffer_t *buf;
    char local[PATH_MAX];
    if (!lspUriToPath(uri, local, sizeof(local))) return;
    count = cJSON_GetArraySize(edits_array);
    if (count <= 0) return;

    edits = gcalloc((size_t) count, sizeof(FlatTextEdit));
    if (!edits) return;
    n = 0;
    for (i = 0; i < count; i++)
    {
        e = cJSON_GetArrayItem(edits_array, i);
        range = cJSON_GetObjectItem(e, "range");
        new_text = cJSON_GetObjectItem(e, "newText");
        start = range ? cJSON_GetObjectItem(range, "start") : NULL;
        end = range ? cJSON_GetObjectItem(range, "end") : NULL;
        if (!start || !end || !new_text || !cJSON_IsString(new_text)) continue;

        edits[n].start_line = cJSON_GetObjectItem(start, "line")->valueint;
        edits[n].start_col = cJSON_GetObjectItem(start, "character")->valueint;
        edits[n].end_line = cJSON_GetObjectItem(end, "line")->valueint;
        edits[n].end_col = cJSON_GetObjectItem(end, "character")->valueint;
        edits[n].new_text = utilStrdup(new_text->valuestring);
        n++;
    }
    qsort(edits, (size_t) n, sizeof(FlatTextEdit), flatTextEditCmpDescending);

    buffer_id = lsp_find_buffer_id_by_uri(uri);
    if (buffer_id < 0)
    {
        path = stringToUtf8((unsigned char *) local);
        buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
        gfree(path);
    }
    buf = bufferFindById(buffer_id);
    if (buf)
    {
        for (i = 0; i < n; i++)
            bufferApplyTextEdit(buf, edits[i].start_line, edits[i].start_col,
                                edits[i].end_line, edits[i].end_col, edits[i].new_text);
    }

    for (i = 0; i < n; i++) gfree(edits[i].new_text);
    gfree(edits);
}

/**
 * @brief Applies a WorkspaceEdit — both its "changes" ({uri: TextEdit[]})
 * and its "documentChanges" (TextDocumentEdit[]) shapes — across every
 * affected buffer, opening files that aren't open yet. Nothing is saved.
 * @return the number of documents touched.
 */
static int applyWorkspaceEdit(const cJSON *edit)
{
    int files_touched;
    cJSON *changes;
    cJSON *edits_array;
    cJSON *document_changes;
    cJSON *entry;
    cJSON *text_doc;
    cJSON *uri_item;
    files_touched = 0;
    if (!edit || !cJSON_IsObject(edit))
        return 0;

    changes = cJSON_GetObjectItem(edit, "changes");
    if (changes)
    {
        // cJSON objects are linked lists of (string key, value) pairs —
        // iterating like this hits the URI keys directly, no key
        // enumeration API needed.
        for (edits_array = changes->child; edits_array; edits_array = edits_array->next)
        {
            applyTextEditsToUri(edits_array->string, edits_array);
            files_touched++;
        }
    }

    document_changes = cJSON_GetObjectItem(edit, "documentChanges");
    if (document_changes && cJSON_IsArray(document_changes))
    {
        cJSON_ArrayForEach(entry, document_changes)
        {
            // Create/rename/delete-file operations carry no "edits".
            text_doc = cJSON_GetObjectItem(entry, "textDocument");
            uri_item = text_doc ? cJSON_GetObjectItem(text_doc, "uri") : NULL;
            edits_array = cJSON_GetObjectItem(entry, "edits");
            if (uri_item && cJSON_IsString(uri_item) && edits_array)
            {
                applyTextEditsToUri(uri_item->valuestring, edits_array);
                files_touched++;
            }
        }
    }
    return files_touched;
}

static void lsp_on_rename_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *result;
    int files_touched;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;

    result = cJSON_GetObjectItem(root, "result");
    if (!result)
    {
        editorSetStatusMessage("Rename failed (no response from server)");
        cJSON_Delete(root);
        return;
    }

    files_touched = applyWorkspaceEdit(result);
    if (files_touched > 0)
        editorSetStatusMessage("Renamed — %d file%s touched (review and save)",
                               files_touched, files_touched == 1 ? "" : "s");
    else
        editorSetStatusMessage("Rename: nothing to change");

    cJSON_Delete(root);
}

// ─── Code actions ──────────────────────────────────────────────────────────

// The last textDocument/codeAction result (an array of Command |
// CodeAction), kept until the user picks one from the popup.
static cJSON *g_code_actions = NULL;

static void lsp_on_execute_command_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *error;
    cJSON *message;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;
    error = cJSON_GetObjectItem(root, "error");
    message = error ? cJSON_GetObjectItem(error, "message") : NULL;
    if (message && cJSON_IsString(message))
        editorSetStatusMessage("Code action failed: %s", message->valuestring);
    cJSON_Delete(root);
}

/**
 * @brief workspace/executeCommand for a Command {title, command,
 * arguments}. The server answers by sending us a workspace/applyEdit
 * request — see lsp_handle_server_request().
 */
static void lsp_execute_command(LspClient *lsp, const cJSON *command)
{
    cJSON *name;
    cJSON *arguments;
    cJSON *req;
    cJSON *params;
    char *msg;
    int req_id;
    name = cJSON_GetObjectItem(command, "command");
    if (!name || !cJSON_IsString(name))
        return;
    arguments = cJSON_GetObjectItem(command, "arguments");

    req_id = lsp->next_id++;
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "workspace/executeCommand");
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "command", name->valuestring);
    if (arguments)
        cJSON_AddItemToObject(params, "arguments", cJSON_Duplicate(arguments, 1));
    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_execute_command_response);
    gfree(msg);
    cJSON_Delete(req);
}

void lspApplyCodeAction(LspClient *lsp, const int index)
{
    cJSON *action;
    cJSON *edit;
    cJSON *command;
    if (!lsp || lsp->state != LSP_READY || !g_code_actions)
        return;
    action = cJSON_GetArrayItem(g_code_actions, index);
    if (!action)
        return;

    command = cJSON_GetObjectItem(action, "command");
    if (command && cJSON_IsString(command))
    {
        // A bare Command — the whole item is the command.
        lsp_execute_command(lsp, action);
    }
    else
    {
        // A CodeAction: its edit first, then its command, if any (spec order).
        edit = cJSON_GetObjectItem(action, "edit");
        if (edit)
            applyWorkspaceEdit(edit);
        if (command && cJSON_IsObject(command))
            lsp_execute_command(lsp, command);
    }

    cJSON_Delete(g_code_actions);
    g_code_actions = NULL;
}

static void lsp_on_code_action_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *result;
    cJSON *title;
    Lisp items;
    int i;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;

    result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0)
    {
        editorSetStatusMessage("No code actions here");
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(g_code_actions);
    g_code_actions = cJSON_DetachItemFromObject(root, "result");

    // (title . index) pairs, built back to front so they keep the
    // server's order — clangd lists quick fixes before refactorings.
    items = lisp_null();
    for (i = cJSON_GetArraySize(g_code_actions) - 1; i >= 0; i--)
    {
        title = cJSON_GetObjectItem(cJSON_GetArrayItem(g_code_actions, i), "title");
        items = lisp_cons(lisp_cons(lisp_make_string2(
                                        (title && cJSON_IsString(title)) ? title->valuestring : "?",
                                        E.ctx),
                                    lisp_make_int(i), E.ctx),
                          items, E.ctx);
    }
    listPopupOpen("Code actions", items, lisp_make_func(lspApplyCodeAction_L), lisp_null(), 0);
    cJSON_Delete(root);
}

/**
 * @brief Answers a request the server sent us. Only workspace/applyEdit
 * (how clangd's tweaks, run through workspace/executeCommand, deliver
 * their edit) and workspace/configuration need real answers; the
 * registration/progress bookkeeping a server may ask for gets a null
 * result, and anything else "method not found" — a server blocks waiting
 * on every one of these, so none may go unanswered.
 */
static void lsp_handle_server_request(LspClient *lsp, const cJSON *id, const char *method,
                                      const cJSON *params)
{
    cJSON *result;
    cJSON *error;
    cJSON *items;
    int applied;
    int i;
    if (strcmp(method, "workspace/applyEdit") == 0)
    {
        applied = params && applyWorkspaceEdit(cJSON_GetObjectItem(params, "edit")) > 0;
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "applied", applied);
        lsp_send_response(lsp, id, result, NULL);
    }
    else if (strcmp(method, "workspace/configuration") == 0)
    {
        result = cJSON_CreateArray();
        items = params ? cJSON_GetObjectItem(params, "items") : NULL;
        for (i = 0; items && i < cJSON_GetArraySize(items); i++)
            cJSON_AddItemToArray(result, cJSON_CreateNull());
        lsp_send_response(lsp, id, result, NULL);
    }
    else if (strcmp(method, "window/workDoneProgress/create") == 0 ||
             strcmp(method, "client/registerCapability") == 0 ||
             strcmp(method, "client/unregisterCapability") == 0)
    {
        lsp_send_response(lsp, id, NULL, NULL);
    }
    else
    {
        error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", -32601);
        cJSON_AddStringToObject(error, "message", "Method not found");
        lsp_send_response(lsp, id, NULL, error);
    }
}

static struct buffer_t *g_format_target_buf = NULL;

static void lsp_on_formatting_response(LspClient *lsp, const char *result_json)
{
    cJSON *root;
    cJSON *result;
    int count;
    FlatTextEdit *edits;
    int n;
    int i;
    cJSON *e;
    cJSON *range;
    cJSON *new_text;
    cJSON *start;
    cJSON *end;
    (void) lsp;
    root = cJSON_Parse(result_json);
    if (!root) return;

    result = cJSON_GetObjectItem(root, "result");
    if (result && cJSON_IsArray(result) && g_format_target_buf)
    {
        count = cJSON_GetArraySize(result);
        edits = count > 0 ? gcalloc((size_t) count, sizeof(FlatTextEdit)) : NULL;
        n = 0;
        if (edits)
        {
            for (i = 0; i < count; i++)
            {
                e = cJSON_GetArrayItem(result, i);
                range = cJSON_GetObjectItem(e, "range");
                new_text = cJSON_GetObjectItem(e, "newText");
                start = range ? cJSON_GetObjectItem(range, "start") : NULL;
                end = range ? cJSON_GetObjectItem(range, "end") : NULL;
                if (!start || !end || !new_text || !cJSON_IsString(new_text)) continue;

                edits[n].start_line = cJSON_GetObjectItem(start, "line")->valueint;
                edits[n].start_col = cJSON_GetObjectItem(start, "character")->valueint;
                edits[n].end_line = cJSON_GetObjectItem(end, "line")->valueint;
                edits[n].end_col = cJSON_GetObjectItem(end, "character")->valueint;
                edits[n].new_text = utilStrdup(new_text->valuestring);
                n++;
            }
            qsort(edits, (size_t) n, sizeof(FlatTextEdit), flatTextEditCmpDescending);
            for (i = 0; i < n; i++)
                bufferApplyTextEdit(g_format_target_buf, edits[i].start_line, edits[i].start_col,
                                    edits[i].end_line, edits[i].end_col, edits[i].new_text);
            for (i = 0; i < n; i++) gfree(edits[i].new_text);
            gfree(edits);
        }
    }
    g_format_target_buf = NULL;
    cJSON_Delete(root);
}

/**
 * @brief Dispatches one fully-parsed JSON-RPC message: either a response
 * (matched by id to a pending request) or a server-initiated notification.
 */
void lspHandleMessage(LspClient *lsp, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *id;
    cJSON *method;
    cJSON *params;
    void (*callback)(LspClient*, const char*);
    const char *method_str;
    cJSON *uri_item;
    int buffer_id;
    if (!root) return;

    id = cJSON_GetObjectItem(root, "id");
    method = cJSON_GetObjectItem(root, "method");
    params = cJSON_GetObjectItem(root, "params");

    if (id && method && cJSON_IsString(method))
    {
        // A request *from* the server: it has both an id (from its own
        // numbering, unrelated to ours) and a method.
        lsp_handle_server_request(lsp, id, method->valuestring, params);
    }
    else if (id && cJSON_IsNumber(id))
    {
        // A response to something we sent — dispatch by id, not by method
        callback = lsp_pop_pending(lsp, id->valueint);
        if (callback)
            callback(lsp, json); // full message — lspParseCompletion pulls "result" itself
    }
    else if (method && cJSON_IsString(method))
    {
        // A genuine server-initiated notification
        method_str = method->valuestring;
        if (strcmp(method_str, "textDocument/publishDiagnostics") == 0 && params)
        {
            uri_item = cJSON_GetObjectItem(params, "uri");
            if (uri_item && cJSON_IsString(uri_item))
            {
                lsp_cache_diagnostics(uri_item->valuestring,
                                      cJSON_GetObjectItem(params, "diagnostics"));
                buffer_id = lsp_find_buffer_id_by_uri(uri_item->valuestring);
                if (buffer_id >= 0)
                    lspParseDiagnostics(json, &E.lsp_diagnostics, buffer_id);
            }
        }
    }

    cJSON_Delete(root);
}


int lspStart(LspClient *lsp, const char *project_root, char *const server_argv[])
{
    OsSpawn spawn;
    const char *debug_log;
    char root_uri[PATH_MAX + 8];
    int init_id;
    cJSON *req;
    cJSON *params;
    cJSON *caps;
    cJSON *td_caps;
    cJSON *comp_caps;
    cJSON *comp_item;
    cJSON *general;
    cJSON *encodings;
    cJSON *hover_caps;
    cJSON *formats;
    cJSON *sig_caps;
    cJSON *sig_info;
    cJSON *param_info;
    cJSON *action_caps;
    cJSON *literal;
    cJSON *kind_caps;
    cJSON *kinds;
    cJSON *ws_caps;
    cJSON *ws_edit;
    char *msg;
    if (!lsp || !project_root || !server_argv || !server_argv[0])
        return -1;

    // stdin/stdout carry the JSON-RPC transport. stderr would otherwise
    // land on the terminal we're drawing on, and language servers log to
    // it freely (clangd emits a line per request at its default level): a
    // normal build discards it, an APP_DEBUG one appends it to the same
    // ~/.beluga/debug.log appDebug() writes to, where it's readable.
    memset(&spawn, 0, sizeof(spawn));
    spawn.argv = (const char *const *) server_argv;
    spawn.stdin_pipe = 1;
    debug_log = appDebugLogPath();
    spawn.stderr_mode = debug_log ? OS_STDERR_FILE : OS_STDERR_DISCARD;
    spawn.stderr_file = debug_log;
    lsp->proc = os_proc_spawn(&spawn);
    if (lsp->proc)
    {
        lsp->next_id = 1;
        lsp->state = LSP_INITIALIZING;
        lsp->pending_count = 0;
        lsp->recv_buf = NULL;
        lsp->recv_len = 0;
        lsp->recv_cap = 0;
        lsp->completion_debounce_pending = 0;
        lsp->completion_debounce_deadline_ms = 0;
        lsp->completion_latest_request_id = 0;

        lspPathToUri(project_root, root_uri, sizeof(root_uri));

        init_id = lsp->next_id++;

        req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(req, "id", init_id);
        cJSON_AddStringToObject(req, "method", "initialize");

        params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "processId", (double) os_getpid());
        cJSON_AddStringToObject(params, "rootUri", root_uri);

        caps = cJSON_CreateObject();
        td_caps = cJSON_CreateObject();
        comp_caps = cJSON_CreateObject();
        comp_item = cJSON_CreateObject();

        cJSON_AddBoolToObject(comp_item, "snippetSupport", 0);
        cJSON_AddBoolToObject(comp_item, "commitCharactersSupport", 0);
        cJSON_AddItemToObject(comp_caps, "completionItem", comp_item);
        cJSON_AddItemToObject(td_caps, "completion", comp_caps);
        // Plain text first: hover is shown as-is in a popup, and markdown
        // would leave it full of ``` fences and ### headings.
        hover_caps = cJSON_CreateObject();
        formats = cJSON_CreateArray();
        cJSON_AddItemToArray(formats, cJSON_CreateString("plaintext"));
        cJSON_AddItemToArray(formats, cJSON_CreateString("markdown"));
        cJSON_AddItemToObject(hover_caps, "contentFormat", formats);
        cJSON_AddItemToObject(td_caps, "hover", hover_caps);

        sig_caps = cJSON_CreateObject();
        sig_info = cJSON_CreateObject();
        param_info = cJSON_CreateObject();
        cJSON_AddBoolToObject(param_info, "labelOffsetSupport", 1);
        cJSON_AddItemToObject(sig_info, "parameterInformation", param_info);
        formats = cJSON_CreateArray();
        cJSON_AddItemToArray(formats, cJSON_CreateString("plaintext"));
        cJSON_AddItemToObject(sig_info, "documentationFormat", formats);
        cJSON_AddBoolToObject(sig_info, "activeParameterSupport", 1);
        cJSON_AddItemToObject(sig_caps, "signatureInformation", sig_info);
        cJSON_AddItemToObject(td_caps, "signatureHelp", sig_caps);

        // Literal CodeActions (with their edit inline) rather than bare
        // Commands — without this clangd answers with commands only, and
        // every quick fix becomes an extra executeCommand round-trip.
        action_caps = cJSON_CreateObject();
        literal = cJSON_CreateObject();
        kind_caps = cJSON_CreateObject();
        kinds = cJSON_CreateArray();
        cJSON_AddItemToArray(kinds, cJSON_CreateString("quickfix"));
        cJSON_AddItemToArray(kinds, cJSON_CreateString("refactor"));
        cJSON_AddItemToArray(kinds, cJSON_CreateString("source"));
        cJSON_AddItemToObject(kind_caps, "valueSet", kinds);
        cJSON_AddItemToObject(literal, "codeActionKind", kind_caps);
        cJSON_AddItemToObject(action_caps, "codeActionLiteralSupport", literal);
        cJSON_AddItemToObject(td_caps, "codeAction", action_caps);
        cJSON_AddItemToObject(td_caps, "definition", cJSON_CreateObject());
        cJSON_AddItemToObject(td_caps, "publishDiagnostics", cJSON_CreateObject());
        cJSON_AddItemToObject(caps, "textDocument", td_caps);

        // workspace/applyEdit is how commands (clangd's tweaks) hand back
        // their edits — see lsp_handle_server_request().
        ws_caps = cJSON_CreateObject();
        cJSON_AddBoolToObject(ws_caps, "applyEdit", 1);
        ws_edit = cJSON_CreateObject();
        cJSON_AddBoolToObject(ws_edit, "documentChanges", 1);
        cJSON_AddItemToObject(ws_caps, "workspaceEdit", ws_edit);
        cJSON_AddItemToObject(caps, "workspace", ws_caps);

        // Request UTF-8 code-point positions instead of UTF-16 code units
        // (LSP 3.17+, clangd 15+ support this) — this matches your Utf8Char
        // model exactly, so a "character" position == an index into
        // row->characters[], with no UTF-16 surrogate-pair conversion needed.
        general = cJSON_CreateObject();
        encodings = cJSON_CreateArray();
        cJSON_AddItemToArray(encodings, cJSON_CreateString("utf-8"));
        cJSON_AddItemToObject(general, "positionEncodings", encodings);
        cJSON_AddItemToObject(caps, "general", general);

        cJSON_AddItemToObject(params, "capabilities", caps);
        cJSON_AddItemToObject(req, "params", params);

        msg = cJSON_PrintUnformatted(req);
        lsp_send(lsp, msg);
        lsp_track_pending(lsp, init_id, lsp_on_initialize_response);

        gfree(msg);
        cJSON_Delete(req);

        return 0;
    }

    return -1;
}

/**
 * @brief Builds the "file://<path>" URI for `buf` into `uri`.
 * @details Every LSP request needs this, and each site used to inline
 * `snprintf(uri, ..., "file://%s", (char*) utf8ToString(buf->fullname))`,
 * which had two problems repeated seven times over: it leaked the string
 * utf8ToString() mallocs (only lspDidClose() freed it), and it walked
 * straight into utf8ToString()'s unguarded `while (*string->character)`
 * when fullname was NULL. That isn't hypothetical — a scratch
 * EXPLORER_BUFF (the explorer sidebar, "*search*", "*tree*") has fullname
 * NULL by design, so pressing CTRL-x h with the search results focused
 * was a null dereference.
 * @return 1 on success, 0 if this buffer has no backing file — callers
 * bail on 0 rather than send the server a request for a document that
 * doesn't exist.
 */
static int lspBufferUri(const struct buffer_t *buf, char *uri, size_t uri_size)
{
    char *full;
    if (!buf || !buf->fullname)
        return 0;

    full = (char *) utf8ToString(buf->fullname);
    if (!full)
        return 0;

    lspPathToUri(full, uri, uri_size);
    gfree(full);
    return 1;
}

void lspDidOpen(LspClient* lsp, struct buffer_t* buf)
{
    char uri[PATH_MAX + 8];
    char* text;
    struct lspServerConfig_t* file_cfg;
    const char* language_id;
    cJSON* root;
    cJSON* params;
    cJSON* textDoc;
    char* msg;
    if (!lsp || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    text = bufferToText(buf);
    if (!text) return;

    // The buffer's OWN registered server config, not lsp->language_id (the
    // whole client's session-wide value, set once from whichever file
    // started the server). Several extensions can share one real server
    // with a different language id each — that's the entire point of the
    // nodejs package registering typescript-language-server under
    // "javascript"/"typescript"/"javascriptreact"/"typescriptreact" — and
    // this used to send every file after the first under the *first*
    // file's language id regardless. For a server that dispatches parsing
    // on languageId (typescript-language-server does — it's exactly how
    // it tells .ts's type annotations from .js), that turned into TS
    // syntax reported as JS syntax errors the moment a JS file happened
    // to be opened first in the session.
    file_cfg = lspServerConfigFor(buf->fullname);
    language_id = (file_cfg && file_cfg->language_id[0])
                                   ? file_cfg->language_id
                                   : (lsp->language_id[0] ? lsp->language_id : "c");

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "textDocument/didOpen");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddStringToObject(textDoc, "languageId", language_id);
    cJSON_AddNumberToObject(textDoc, "version", 1);
    cJSON_AddStringToObject(textDoc, "text", text);
    cJSON_AddItemToObject(params, "textDocument", textDoc);
    cJSON_AddItemToObject(root, "params", params);

    msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);

    gfree(text);
    gfree(msg);
    cJSON_Delete(root);
}

void lspDidChange(LspClient* lsp, struct buffer_t* buf)
{
    char uri[PATH_MAX + 8];
    char* text;
    cJSON* root;
    cJSON* params;
    cJSON* textDoc;
    cJSON* changes;
    cJSON* change;
    char* msg;
    if (!lsp || !buf || lsp->state != LSP_READY)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    text = bufferToText(buf);
    if (!text) return;

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "textDocument/didChange");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddNumberToObject(textDoc, "version", buf->dirty);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    changes = cJSON_CreateArray();
    change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "text", text); // full-document sync
    cJSON_AddItemToArray(changes, change);
    cJSON_AddItemToObject(params, "contentChanges", changes);

    cJSON_AddItemToObject(root, "params", params);

    msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);

    gfree(text);
    gfree(msg);
    cJSON_Delete(root);
}

void lspDidClose(LspClient* lsp, struct buffer_t* buf)
{
    char uri[PATH_MAX + 8];
    cJSON* root;
    cJSON* params;
    cJSON* textDoc;
    char* msg;
    if (!lsp || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "textDocument/didClose");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);
    cJSON_AddItemToObject(root, "params", params);

    msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);

    gfree(msg);
    cJSON_Delete(root);
}

void lspRequestCompletion(LspClient *lsp, const struct buffer_t *buf,
                          const int line, const int col,
                          const int screen_x, const int screen_y)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* td;
    cJSON* position;
    char* msg;
    if (!lsp || lsp->state != LSP_READY) return;

    lsp->completion_cursor_x = screen_x;
    lsp->completion_cursor_y = screen_y;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    params = cJSON_CreateObject();
    td = cJSON_CreateObject();
    position = cJSON_CreateObject();

    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/completion");

    cJSON_AddStringToObject(td, "uri", uri);

    // `col` here is a raw index into row->characters[] — valid directly as
    // an LSP "character" position IF the server accepted our utf-8
    // positionEncoding capability. If it didn't (older clangd), this needs
    // a UTF-8 -> UTF-16 code-unit conversion instead — flagging this as a
    // thing to verify once you can see completions actually land in the
    // right place for lines containing multi-byte characters.
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);

    cJSON_AddItemToObject(params, "textDocument", td);
    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_completion_response);
    lsp->completion_requested = 1;
    lsp->completion_latest_request_id = req_id;

    cJSON_Delete(req);
    gfree(msg);
}

void lspRequestDefinition(LspClient *lsp, struct buffer_t *buf,
                          const int line, const int col)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* textDoc;
    cJSON* position;
    char* msg;
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    lsp_sync_if_dirty(lsp, buf);

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/definition");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);

    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_definition_response);

    gfree(msg);
    cJSON_Delete(req);
}

void lspRequestHover(LspClient *lsp, struct buffer_t *buf, const int line, const int col)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* textDoc;
    cJSON* position;
    char* msg;
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    lsp_sync_if_dirty(lsp, buf);

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/hover");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);

    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_hover_response);

    gfree(msg);
    cJSON_Delete(req);
}

void lspRequestSignatureHelp(LspClient *lsp, struct buffer_t *buf, const int line, const int col)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* textDoc;
    cJSON* position;
    char* msg;
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    // Usually fired right after "(" or "," was typed: the server has to
    // see that keystroke to know which call and argument the cursor is in.
    lsp_sync_if_dirty(lsp, buf);

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/signatureHelp");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);

    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_signature_help_response);

    gfree(msg);
    cJSON_Delete(req);
}

void lspRequestCodeAction(LspClient *lsp, struct buffer_t *buf, const int line, const int col)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* textDoc;
    cJSON* range;
    cJSON* pos;
    cJSON* context;
    cJSON* diags;
    const cJSON* cached;
    const cJSON* d;
    cJSON* d_range;
    cJSON* d_start;
    cJSON* d_end;
    cJSON* d_line;
    int start_line;
    int end_line;
    char* msg;
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    lsp_sync_if_dirty(lsp, buf);

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/codeAction");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    // An empty range at the cursor: refactorings (clangd's tweaks) are
    // offered for what the cursor is on, and a whole-line range is a
    // selection most of them refuse. Quick fixes don't depend on it —
    // they come from the diagnostics in the context below.
    range = cJSON_CreateObject();
    pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(pos, "line", line);
    cJSON_AddNumberToObject(pos, "character", col);
    cJSON_AddItemToObject(range, "start", pos);
    pos = cJSON_CreateObject();
    cJSON_AddNumberToObject(pos, "line", line);
    cJSON_AddNumberToObject(pos, "character", col);
    cJSON_AddItemToObject(range, "end", pos);
    cJSON_AddItemToObject(params, "range", range);

    // Quick fixes are only offered for the diagnostics passed back here,
    // so hand over every published one that touches the cursor's line,
    // verbatim — a fix is usually wanted for "the error on this line",
    // wherever on it the cursor happens to be.
    diags = cJSON_CreateArray();
    cached = lsp_cached_diagnostics(uri);
    if (cached && cJSON_IsArray(cached))
    {
        cJSON_ArrayForEach(d, cached)
        {
            d_range = cJSON_GetObjectItem(d, "range");
            d_start = d_range ? cJSON_GetObjectItem(d_range, "start") : NULL;
            d_end = d_range ? cJSON_GetObjectItem(d_range, "end") : NULL;
            d_line = d_start ? cJSON_GetObjectItem(d_start, "line") : NULL;
            if (!d_line) continue;
            start_line = d_line->valueint;
            d_line = d_end ? cJSON_GetObjectItem(d_end, "line") : NULL;
            end_line = d_line ? d_line->valueint : start_line;
            if (start_line <= line && line <= end_line)
                cJSON_AddItemToArray(diags, cJSON_Duplicate(d, 1));
        }
    }
    context = cJSON_CreateObject();
    cJSON_AddItemToObject(context, "diagnostics", diags);
    cJSON_AddItemToObject(params, "context", context);

    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_code_action_response);

    gfree(msg);
    cJSON_Delete(req);
}

/**
 * @brief Inserts the chosen completion over what's already typed.
 * @details The server's textEdit range starts where the word being
 * completed starts, which is the only reliable way to know how much of it
 * is already in the buffer: with fuzzy matching, "prf" completes to
 * "printf", so skipping strlen("prf") bytes of the insert text — what this
 * used to do — produced "prfntf". The range was computed when the request
 * went out; anything typed since then sits between its end and the
 * cursor, so the replacement runs up to the cursor (or past it, for an
 * InsertReplaceEdit's "replace" range covering the rest of the word).
 */
void lspAcceptCompletion(struct buffer_t *buf, const CompletionItem *item)
{
    Row *row;
    int start_col;
    int end_col;
    if (!buf || !item || buf->y < 0 || buf->y >= buf->numrows)
        return;
    row = &buf->row[buf->y];
    end_col = buf->x;

    if (item->has_edit && item->edit_line == buf->y && item->edit_start_col <= buf->x)
    {
        start_col = item->edit_start_col;
        if (item->edit_end_col > end_col)
            end_col = item->edit_end_col;
    }
    else
    {
        // No range from the server: replace the word before the cursor.
        // is_word_char() counts '#' as a word char (for "#include"
        // highlighting), but a preprocessor completion's text never
        // includes it — stop right after one.
        start_col = buf->x;
        while (start_col > 0 &&
               is_word_char(&row->characters[start_col - 1]) &&
               row->characters[start_col - 1].character[0] != '#')
            start_col--;
    }
    if (end_col > row->size)
        end_col = row->size;

    bufferApplyTextEdit(buf, buf->y, start_col, buf->y, end_col, item->insertText);
}

void lspRequestRename(LspClient *lsp, struct buffer_t *buf, const int line, const int col,
                      const char *new_name)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* textDoc;
    cJSON* position;
    char* msg;
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/rename");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);
    cJSON_AddStringToObject(params, "newName", new_name);

    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_rename_response);
    lspAwaitResponse(lsp, req_id, 3000);

    gfree(msg);
    cJSON_Delete(req);
}

void lspRequestFormatting(LspClient *lsp, struct buffer_t *buf)
{
    char uri[PATH_MAX + 8];
    int req_id;
    cJSON* req;
    cJSON* params;
    cJSON* textDoc;
    cJSON* options;
    char* msg;
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    if (!lspBufferUri(buf, uri, sizeof(uri)))
        return;

    // The server computes edit ranges against ITS copy of the document, and
    // ours is only pushed on the completion debounce — so without an
    // explicit resync here, any edit made since the last completion request
    // leaves the server formatting stale text, and the returned ranges then
    // land at the wrong offsets in our buffer.
    lspDidChange(lsp, buf);

    req_id = lsp->next_id++;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/formatting");

    params = cJSON_CreateObject();
    textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    options = cJSON_CreateObject();
    cJSON_AddNumberToObject(options, "tabSize", E.constantes.TAB_LENGTH);
    cJSON_AddBoolToObject(options, "insertSpaces", 1);
    cJSON_AddItemToObject(params, "options", options);

    cJSON_AddItemToObject(req, "params", params);

    msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    g_format_target_buf = buf;
    lsp_track_pending(lsp, req_id, lsp_on_formatting_response);
    if (!lspAwaitResponse(lsp, req_id, 3000))
    {
        // Timed out. Clear the target so a late reply — arriving from the
        // normal run-loop poll, by which point the buffer may have been
        // edited further or closed outright — can't apply edits computed
        // for a document state that no longer exists.
        g_format_target_buf = NULL;
        editorSetStatusMessage("Formatting timed out");
    }

    gfree(msg);
    cJSON_Delete(req);
}

int lspAwaitResponse(LspClient *lsp, const int request_id, const int timeout_ms)
{
    long deadline;
    int still_pending;
    int i;
    OsStream *out[1];
    int ready;
    if (!lsp || !lsp->proc)
        return 0;

    deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline && lsp->proc)
    {
        still_pending = 0;
        for (i = 0; i < lsp->pending_count; i++)
        {
            if (lsp->pending[i].id == request_id) { still_pending = 1; break; }
        }
        if (!still_pending)
            return 1;

        out[0] = os_proc_output(lsp->proc);
        if (os_wait(out, 1, 50, &ready) > 0)
            lspPoll(lsp);
    }
    return 0;
}

// ─── Completion debounce ───────────────────────────────────────────────────
//
// editorProcessKeypress() used to call lspDidChange + lspRequestCompletion
// synchronously on every word-char keystroke — a full-document resync plus
// a completion round-trip per character typed, with no way to drop a
// request once a newer one made it stale. These three functions replace
// that: the input path just (re)starts an idle timer, and the actual
// request only goes out once typing pauses for LSP_COMPLETION_DEBOUNCE_MS.

/**
 * @brief Sends the actual didChange+completion round-trip for whichever
 * buffer is active right now (recomputed at fire time, not at the moment
 * the keystroke that scheduled us happened, so it always reflects the
 * current cursor position).
 */
static void lsp_fire_completion_now(void)
{
    EditorPane *active;
    struct buffer_t *buf;
    if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY)
        return;

    active = splitScreenGetActivePane();
    buf = bufferFindById(active->buffer_id);
    if (!buf || buf->type != FILE_BUFF || !buf->b_lsp_open)
        return;

    lspDidChange(E.lsp_client, buf);
    buf->lsp_synced_dirty = buf->dirty;
    lspRequestCompletion(E.lsp_client, buf, buf->y, buf->x,
                          active->cursor_x + active->origin_x + editorGutterWidth(buf),
                          active->cursor_y + active->origin_y);
}

void lspScheduleCompletion(void)
{
    if (!E.lsp_client) return;
    E.lsp_client->completion_debounce_pending = 1;
    E.lsp_client->completion_debounce_deadline_ms = monotonic_ms() + LSP_COMPLETION_DEBOUNCE_MS;
}

void lspCancelScheduledCompletion(void)
{
    if (!E.lsp_client) return;
    E.lsp_client->completion_debounce_pending = 0;
}

/**
 * @brief Call once per editorRunLoop iteration. A no-op unless a completion
 * was scheduled and its idle deadline has passed.
 */
void lspCompletionDebounceTick(void)
{
    if (!E.lsp_client || !E.lsp_client->completion_debounce_pending)
        return;
    if (monotonic_ms() < E.lsp_client->completion_debounce_deadline_ms)
        return;

    E.lsp_client->completion_debounce_pending = 0;
    lsp_fire_completion_now();
}

/**
 * @brief Call once per editorRunLoop iteration, alongside
 * lspCompletionDebounceTick(). A no-op unless the active buffer's `dirty`
 * counter has moved past what was last sent to the server.
 * @details Exists because completion is armed only by word-char/trigger-char
 * keystrokes (see should_trigger_completion in input.c) — deleting text,
 * undo/redo, paste, and kill-region never go through that path, so without
 * this they'd never resync the document at all, and the server would keep
 * reporting diagnostics against stale content (e.g. an "unused include"
 * hint for a #include line that's already been deleted) until some
 * unrelated keystroke elsewhere happened to trigger completion. Debounced
 * the same way completion is, just against buf->dirty instead of an
 * explicit schedule call, so it needs no call site of its own anywhere a
 * buffer might be edited.
 */
void lspSyncDebounceTick(void)
{
    EditorPane *active;
    struct buffer_t *buf;
    long now;
    if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY)
        return;

    active = splitScreenGetActivePane();
    buf = bufferFindById(active->buffer_id);
    if (!buf || buf->type != FILE_BUFF || !buf->b_lsp_open)
        return;

    if (buf->dirty == buf->lsp_synced_dirty) {
        E.lsp_client->sync_debounce_pending = 0;
        return;
    }

    now = monotonic_ms();
    if (!E.lsp_client->sync_debounce_pending ||
        E.lsp_client->sync_debounce_buffer_id != buf->buffer_id) {
        E.lsp_client->sync_debounce_pending = 1;
        E.lsp_client->sync_debounce_buffer_id = buf->buffer_id;
        E.lsp_client->sync_debounce_deadline_ms = now + LSP_SYNC_DEBOUNCE_MS;
        return;
    }

    if (now < E.lsp_client->sync_debounce_deadline_ms)
        return;

    lspDidChange(E.lsp_client, buf);
    buf->lsp_synced_dirty = buf->dirty;
    E.lsp_client->sync_debounce_pending = 0;
}

/**
 * @brief Called from editorRunLoop() when the server has written something:
 * drains whatever's available, without waiting, into the incremental
 * parser.
 */
void lspPoll(LspClient *lsp)
{
    char chunk[4096];
    long n;
    if (!lsp || !lsp->proc)
        return;

    while ((n = os_stream_read(os_proc_output(lsp->proc), chunk, sizeof(chunk), 0)) > 0)
        lsp_feed(lsp, chunk, (size_t) n);
    if (n < 0)
    {
        // Its output has ended: the server exited (or crashed). Release it,
        // so the main loop stops waiting on a stream that will never
        // produce anything again.
        os_proc_free(lsp->proc);
        lsp->proc = NULL;
        lsp->state = LSP_SHUTDOWN;
        appDebug("[LSP] server exited\n");
    }
}

void lspShutdown(LspClient *lsp)
{
    int first_buffer_id;
    struct buffer_t* b;
    cJSON *root;
    char *msg;
    int timeout;
    if (!lsp)
        return;

    if (E.buffers != NULL)
    {
        first_buffer_id = E.buffers->buffer_id;
        b = E.buffers;
        do
        {
            if (b->b_lsp_open)
            {
                // Symmetric with the didOpen dedup: only send didClose
                // once every buffer sharing this URI has been accounted
                // for, not once per buffer. Checked before zeroing this
                // buffer's own flag, so whichever buffer is processed
                // last for a given URI is the one that actually closes
                // it, regardless of iteration order.
                if (bufferFindOtherLspOpen(b) == -1)
                    lspDidClose(lsp, b);
                b->b_lsp_open = 0;
            }
            b = b->next;
        } while (b->buffer_id != first_buffer_id);
    }

    if (lsp->state == LSP_READY)
    {
        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(root, "id", lsp->next_id++);
        cJSON_AddStringToObject(root, "method", "shutdown");

        msg = cJSON_PrintUnformatted(root);
        lsp_send(lsp, msg);
        gfree(msg);
        cJSON_Delete(root);

        lsp->state = LSP_SHUTTING_DOWN;

        timeout = 1000;
        while (lsp->state == LSP_SHUTTING_DOWN && timeout > 0)
        {
            os_sleep_ms(10);
            timeout -= 10;
            lspPoll(lsp);
        }

        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "jsonrpc", "2.0");
        cJSON_AddStringToObject(root, "method", "exit");

        msg = cJSON_PrintUnformatted(root);
        lsp_send(lsp, msg);
        gfree(msg);
        cJSON_Delete(root);
    }

    lsp->state = LSP_SHUTDOWN;

    // Only ever reached from lspCleanup() at exit, after the main loop —
    // never while lsp_feed() is parsing this buffer — so it's safe to free.
    gfree(lsp->recv_buf);
    lsp->recv_buf = NULL;
    lsp_free_diag_cache();
    cJSON_Delete(g_code_actions);
    g_code_actions = NULL;
    lsp->recv_len = 0;
    lsp->recv_cap = 0;

    if (lsp->proc)
    {
        // It was just told to exit: give it 100ms to do so on its own;
        // os_proc_free() kills it if it's still running.
        os_proc_wait(lsp->proc, 100, NULL);
        os_proc_free(lsp->proc);
        lsp->proc = NULL;
    }
}

void lspCleanup(LspClient *lsp)
{
    if (!lsp)
        return;
    lspShutdown(lsp);
    gfree(lsp);
}
