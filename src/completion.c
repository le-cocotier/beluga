/**
 * @file completion.c
 * @brief LSP (Language Server Protocol) client implementation for Beluga
 */

#include "../include/completion.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include "../include/append_buffer.h"
#include "../include/cJSON.h"
#include "../include/data.h"
#include "../include/lsp_ui.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/editor_op.h"

extern EditorConfig E;

// ─── Static Functions ──────────────────────────────────────────────────────

/**
 * @brief Monotonic clock in milliseconds, used for the completion debounce
 * timer. Immune to wall-clock adjustments, unlike time()/gettimeofday().
 */
static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void lsp_send(LspClient *lsp, const char *json)
{
    int body_len = strlen(json);
    char header[1024];
    int header_len = snprintf(header, sizeof(header),
                              "Content-Length: %d\r\n\r\n", body_len);

    write(lsp->write_to_server_fd, header, header_len);
    write(lsp->write_to_server_fd, json, body_len);

    appDebug("[LSP →] Content-Length: %d | %s\n", body_len, json);
}

static void lsp_on_initialize_response(LspClient *lsp, const char *result_json)
{
    // Pull completionProvider.triggerCharacters out of the server's own
    // capabilities instead of assuming word-char completion covers
    // everything — a server that only ever completes after punctuation
    // (e.g. "." for member access) would otherwise never get asked.
    lsp->num_trigger_chars = 0;
    cJSON *init_root = cJSON_Parse(result_json);
    if (init_root)
    {
        cJSON *result = cJSON_GetObjectItem(init_root, "result");
        cJSON *caps = result ? cJSON_GetObjectItem(result, "capabilities") : NULL;
        cJSON *comp_provider = caps ? cJSON_GetObjectItem(caps, "completionProvider") : NULL;
        cJSON *trig = comp_provider ? cJSON_GetObjectItem(comp_provider, "triggerCharacters") : NULL;
        if (trig && cJSON_IsArray(trig))
        {
            cJSON *item;
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "initialized");
    cJSON_AddItemToObject(root, "params", cJSON_CreateObject());

    char *msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);
    free(msg);
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
        int first_buffer_id = E.buffers->buffer_id;
        struct buffer_t* b = E.buffers;
        do
        {
            if (b->type == FILE_BUFF && !b->b_lsp_open && lspServerConfigFor(b->fullname))
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
            }
            b = b->next;
        } while (b->buffer_id != first_buffer_id);
    }
}

/**
 * @brief Register a callback to be invoked when a response with this id arrives.
 */
static void lsp_track_pending(LspClient *lsp, int id, void (*callback)(LspClient*, const char*))
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
static void (*lsp_pop_pending(LspClient *lsp, int id))(LspClient*, const char*)
{
    for (int i = 0; i < lsp->pending_count; i++)
    {
        if (lsp->pending[i].id == id)
        {
            void (*cb)(LspClient*, const char*) = lsp->pending[i].callback;
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
    size_t needed = lsp->recv_len + len;
    if (needed > lsp->recv_cap)
    {
        size_t new_cap = lsp->recv_cap ? lsp->recv_cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        char *grown = realloc(lsp->recv_buf, new_cap);
        if (!grown) return; // best-effort: drop data rather than crash
        lsp->recv_buf = grown;
        lsp->recv_cap = new_cap;
    }
    memcpy(lsp->recv_buf + lsp->recv_len, data, len);
    lsp->recv_len += len;

    // Try to extract as many complete messages as are currently buffered
    while (1)
    {
        char *header_end = memmem(lsp->recv_buf, lsp->recv_len, "\r\n\r\n", 4);
        if (!header_end) break; // headers not fully arrived yet

        int header_len = (int)(header_end - lsp->recv_buf);
        char header_copy[1024];
        int copy_len = header_len < (int)sizeof(header_copy) - 1 ? header_len : (int)sizeof(header_copy) - 1;
        memcpy(header_copy, lsp->recv_buf, copy_len);
        header_copy[copy_len] = '\0';

        int content_length = 0;
        char *cl = strstr(header_copy, "Content-Length: ");
        if (cl) content_length = atoi(cl + 16);

        int body_start = header_len + 4;
        if (content_length <= 0 || lsp->recv_len < (size_t)(body_start + content_length))
            break; // body not fully arrived yet

        // Full message available — extract and dispatch
        char *body = malloc(content_length + 1);
        if (body)
        {
            memcpy(body, lsp->recv_buf + body_start, content_length);
            body[content_length] = '\0';
            appDebug("[LSP ←] %s\n", body);
            lspHandleMessage(lsp, body);
            free(body);
        }

        // Shift any remaining buffered bytes down to the front
        size_t consumed = body_start + content_length;
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
    if (root)
    {
        cJSON *id_item = cJSON_GetObjectItem(root, "id");
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
 * @brief Maps a `file://...` URI (as sent back by the server in
 * publishDiagnostics) to the buffer_id of the matching open buffer, or -1.
 */
static int lsp_find_buffer_id_by_uri(const char *uri)
{
    if (E.buffers == NULL) return -1;
    int first_buffer_id = E.buffers->buffer_id;
    struct buffer_t *b = E.buffers;
    do
    {
        if (b->type == FILE_BUFF)
        {
            char buf_uri[PATH_MAX + 8];
            char *fullname_str = (char*) utf8ToString(b->fullname);
            snprintf(buf_uri, sizeof(buf_uri), "file://%s", fullname_str);
            free(fullname_str);

            if (strcmp(buf_uri, uri) == 0)
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
static void lspJumpToUri(const char *uri, int line, int character)
{
    if (strncmp(uri, "file://", 7) != 0)
        return; // not a local file — nothing this editor can open

    int buffer_id = lsp_find_buffer_id_by_uri(uri);
    if (buffer_id < 0)
    {
        Utf8Char *path = stringToUtf8((unsigned char *) uri + 7);
        buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
        free(path);
        if (buffer_id < 0)
        {
            editorSetStatusMessage("Couldn't open %s", uri + 7);
            return;
        }
    }
    else
    {
        bufferSwitch(buffer_id);
    }

    struct buffer_t *buf = bufferFindById(buffer_id);
    if (!buf) return;

    buf->y = (line < 0) ? 0 : (line >= buf->numrows ? (buf->numrows > 0 ? buf->numrows - 1 : 0) : line);
    buf->x = (character < 0) ? 0 : character;
    if (buf->numrows > 0 && buf->x > buf->row[buf->y].size)
        buf->x = buf->row[buf->y].size;
}

static void lsp_on_definition_response(LspClient *lsp, const char *result_json)
{
    (void) lsp;
    cJSON *root = cJSON_Parse(result_json);
    if (!root) return;

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *loc = NULL;
    if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0)
        loc = cJSON_GetArrayItem(result, 0);
    else if (result && cJSON_IsObject(result))
        loc = result;

    int jumped = 0;
    if (loc)
    {
        // Location has uri/range; LocationLink (what clangd actually sends)
        // has targetUri/targetRange instead.
        cJSON *uri_item = cJSON_GetObjectItem(loc, "uri");
        cJSON *range = cJSON_GetObjectItem(loc, "range");
        if (!uri_item) uri_item = cJSON_GetObjectItem(loc, "targetUri");
        if (!range) range = cJSON_GetObjectItem(loc, "targetRange");
        cJSON *start = range ? cJSON_GetObjectItem(range, "start") : NULL;
        cJSON *line_item = start ? cJSON_GetObjectItem(start, "line") : NULL;
        cJSON *char_item = start ? cJSON_GetObjectItem(start, "character") : NULL;

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
 * @brief Extracts a single display string from a hover response's
 * `result.contents`, which per spec may be a plain string, a
 * {kind,value} MarkupContent, a single MarkedString, or an array of any
 * of those. Only the first line of whatever comes back is ever shown
 * (editorSetStatusMessage's single-line status bar), so this just needs
 * *a* representative string, not a faithful full rendering.
 */
static const char *lsp_hover_contents_text(cJSON *contents)
{
    if (!contents) return NULL;
    if (cJSON_IsString(contents)) return contents->valuestring;
    if (cJSON_IsArray(contents) && cJSON_GetArraySize(contents) > 0)
        return lsp_hover_contents_text(cJSON_GetArrayItem(contents, 0));
    if (cJSON_IsObject(contents))
    {
        cJSON *value = cJSON_GetObjectItem(contents, "value");
        if (value && cJSON_IsString(value)) return value->valuestring;
    }
    return NULL;
}

static void lsp_on_hover_response(LspClient *lsp, const char *result_json)
{
    (void) lsp;
    cJSON *root = cJSON_Parse(result_json);
    if (!root) return;

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *contents = result ? cJSON_GetObjectItem(result, "contents") : NULL;
    const char *text = lsp_hover_contents_text(contents);

    if (text && text[0])
    {
        char first_line[256];
        int i = 0;
        while (text[i] && text[i] != '\n' && i < (int) sizeof(first_line) - 1)
        {
            first_line[i] = text[i];
            i++;
        }
        first_line[i] = '\0';
        editorSetStatusMessage("%s", first_line);
    }
    else
    {
        editorSetStatusMessage("No hover info");
    }
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
static void applyTextEditsToUri(const char *uri, cJSON *edits_array)
{
    if (strncmp(uri, "file://", 7) != 0) return;
    int count = cJSON_GetArraySize(edits_array);
    if (count <= 0) return;

    FlatTextEdit *edits = calloc((size_t) count, sizeof(FlatTextEdit));
    if (!edits) return;
    int n = 0;
    for (int i = 0; i < count; i++)
    {
        cJSON *e = cJSON_GetArrayItem(edits_array, i);
        cJSON *range = cJSON_GetObjectItem(e, "range");
        cJSON *new_text = cJSON_GetObjectItem(e, "newText");
        cJSON *start = range ? cJSON_GetObjectItem(range, "start") : NULL;
        cJSON *end = range ? cJSON_GetObjectItem(range, "end") : NULL;
        if (!start || !end || !new_text || !cJSON_IsString(new_text)) continue;

        edits[n].start_line = cJSON_GetObjectItem(start, "line")->valueint;
        edits[n].start_col = cJSON_GetObjectItem(start, "character")->valueint;
        edits[n].end_line = cJSON_GetObjectItem(end, "line")->valueint;
        edits[n].end_col = cJSON_GetObjectItem(end, "character")->valueint;
        edits[n].new_text = strdup(new_text->valuestring);
        n++;
    }
    qsort(edits, (size_t) n, sizeof(FlatTextEdit), flatTextEditCmpDescending);

    int buffer_id = lsp_find_buffer_id_by_uri(uri);
    if (buffer_id < 0)
    {
        Utf8Char *path = stringToUtf8((unsigned char *) uri + 7);
        buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
        free(path);
    }
    struct buffer_t *buf = bufferFindById(buffer_id);
    if (buf)
    {
        for (int i = 0; i < n; i++)
            bufferApplyTextEdit(buf, edits[i].start_line, edits[i].start_col,
                                edits[i].end_line, edits[i].end_col, edits[i].new_text);
    }

    for (int i = 0; i < n; i++) free(edits[i].new_text);
    free(edits);
}

static void lsp_on_rename_response(LspClient *lsp, const char *result_json)
{
    (void) lsp;
    cJSON *root = cJSON_Parse(result_json);
    if (!root) return;

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result)
    {
        editorSetStatusMessage("Rename failed (no response from server)");
        cJSON_Delete(root);
        return;
    }

    int files_touched = 0;

    cJSON *changes = cJSON_GetObjectItem(result, "changes");
    if (changes)
    {
        cJSON *edits_array;
        // cJSON objects are linked lists of (string key, value) pairs —
        // iterating like this hits the URI keys directly, no key
        // enumeration API needed.
        for (edits_array = changes->child; edits_array; edits_array = edits_array->next)
        {
            applyTextEditsToUri(edits_array->string, edits_array);
            files_touched++;
        }
    }

    cJSON *document_changes = cJSON_GetObjectItem(result, "documentChanges");
    if (document_changes && cJSON_IsArray(document_changes))
    {
        cJSON *entry;
        cJSON_ArrayForEach(entry, document_changes)
        {
            cJSON *text_doc = cJSON_GetObjectItem(entry, "textDocument");
            cJSON *uri_item = text_doc ? cJSON_GetObjectItem(text_doc, "uri") : NULL;
            cJSON *edits_array = cJSON_GetObjectItem(entry, "edits");
            if (uri_item && cJSON_IsString(uri_item) && edits_array)
            {
                applyTextEditsToUri(uri_item->valuestring, edits_array);
                files_touched++;
            }
        }
    }

    if (files_touched > 0)
        editorSetStatusMessage("Renamed — %d file%s touched (review and save)",
                               files_touched, files_touched == 1 ? "" : "s");
    else
        editorSetStatusMessage("Rename: nothing to change");

    cJSON_Delete(root);
}

static struct buffer_t *g_format_target_buf = NULL;

static void lsp_on_formatting_response(LspClient *lsp, const char *result_json)
{
    (void) lsp;
    cJSON *root = cJSON_Parse(result_json);
    if (!root) return;

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (result && cJSON_IsArray(result) && g_format_target_buf)
    {
        int count = cJSON_GetArraySize(result);
        FlatTextEdit *edits = count > 0 ? calloc((size_t) count, sizeof(FlatTextEdit)) : NULL;
        int n = 0;
        if (edits)
        {
            for (int i = 0; i < count; i++)
            {
                cJSON *e = cJSON_GetArrayItem(result, i);
                cJSON *range = cJSON_GetObjectItem(e, "range");
                cJSON *new_text = cJSON_GetObjectItem(e, "newText");
                cJSON *start = range ? cJSON_GetObjectItem(range, "start") : NULL;
                cJSON *end = range ? cJSON_GetObjectItem(range, "end") : NULL;
                if (!start || !end || !new_text || !cJSON_IsString(new_text)) continue;

                edits[n].start_line = cJSON_GetObjectItem(start, "line")->valueint;
                edits[n].start_col = cJSON_GetObjectItem(start, "character")->valueint;
                edits[n].end_line = cJSON_GetObjectItem(end, "line")->valueint;
                edits[n].end_col = cJSON_GetObjectItem(end, "character")->valueint;
                edits[n].new_text = strdup(new_text->valuestring);
                n++;
            }
            qsort(edits, (size_t) n, sizeof(FlatTextEdit), flatTextEditCmpDescending);
            for (int i = 0; i < n; i++)
                bufferApplyTextEdit(g_format_target_buf, edits[i].start_line, edits[i].start_col,
                                    edits[i].end_line, edits[i].end_col, edits[i].new_text);
            for (int i = 0; i < n; i++) free(edits[i].new_text);
            free(edits);
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
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (id && cJSON_IsNumber(id))
    {
        // A response to something we sent — dispatch by id, not by method
        void (*callback)(LspClient*, const char*) = lsp_pop_pending(lsp, id->valueint);
        if (callback)
            callback(lsp, json); // full message — lspParseCompletion pulls "result" itself
    }
    else if (method && cJSON_IsString(method))
    {
        // A genuine server-initiated notification
        const char *method_str = method->valuestring;
        if (strcmp(method_str, "textDocument/publishDiagnostics") == 0 && params)
        {
            cJSON *uri_item = cJSON_GetObjectItem(params, "uri");
            if (uri_item && cJSON_IsString(uri_item))
            {
                int buffer_id = lsp_find_buffer_id_by_uri(uri_item->valuestring);
                if (buffer_id >= 0)
                    lspParseDiagnostics(json, &E.lsp_diagnostics, buffer_id);
            }
        }
    }

    cJSON_Delete(root);
}


int lspStart(LspClient *client, const char *root_dir, char *const server_argv[])
{
    if (!client || !root_dir || !server_argv || !server_argv[0])
        return -1;

    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1)
        return -1;

    pid_t pid = fork();
    if (pid == 0)
    {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        execvp(server_argv[0], server_argv);
        _exit(127);
    }
    else if (pid > 0)
    {
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        client->write_to_server_fd = stdin_pipe[1];
        client->read_from_server_fd = stdout_pipe[0];
        client->pid = pid;
        client->next_id = 1;
        client->state = LSP_INITIALIZING;
        client->pending_count = 0;
        client->recv_buf = NULL;
        client->recv_len = 0;
        client->recv_cap = 0;
        client->completion_debounce_pending = 0;
        client->completion_debounce_deadline_ms = 0;
        client->completion_latest_request_id = 0;

        // Make the read side non-blocking — it'll be driven by select()
        int flags = fcntl(client->read_from_server_fd, F_GETFL, 0);
        fcntl(client->read_from_server_fd, F_SETFL, flags | O_NONBLOCK);

        char root_uri[PATH_MAX + 8];
        snprintf(root_uri, sizeof(root_uri), "file://%s", root_dir);

        int init_id = client->next_id++;

        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(req, "id", init_id);
        cJSON_AddStringToObject(req, "method", "initialize");

        cJSON *params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "processId", getpid());
        cJSON_AddStringToObject(params, "rootUri", root_uri);

        cJSON *caps = cJSON_CreateObject();
        cJSON *td_caps = cJSON_CreateObject();
        cJSON *comp_caps = cJSON_CreateObject();
        cJSON *comp_item = cJSON_CreateObject();

        cJSON_AddBoolToObject(comp_item, "snippetSupport", 0);
        cJSON_AddBoolToObject(comp_item, "commitCharactersSupport", 0);
        cJSON_AddItemToObject(comp_caps, "completionItem", comp_item);
        cJSON_AddItemToObject(td_caps, "completion", comp_caps);
        cJSON_AddItemToObject(td_caps, "hover", cJSON_CreateObject());
        cJSON_AddItemToObject(td_caps, "definition", cJSON_CreateObject());
        cJSON_AddItemToObject(td_caps, "publishDiagnostics", cJSON_CreateObject());
        cJSON_AddItemToObject(caps, "textDocument", td_caps);

        // Request UTF-8 code-point positions instead of UTF-16 code units
        // (LSP 3.17+, clangd 15+ support this) — this matches your Utf8Char
        // model exactly, so a "character" position == an index into
        // row->characters[], with no UTF-16 surrogate-pair conversion needed.
        cJSON *general = cJSON_CreateObject();
        cJSON *encodings = cJSON_CreateArray();
        cJSON_AddItemToArray(encodings, cJSON_CreateString("utf-8"));
        cJSON_AddItemToObject(general, "positionEncodings", encodings);
        cJSON_AddItemToObject(caps, "general", general);

        cJSON_AddItemToObject(params, "capabilities", caps);
        cJSON_AddItemToObject(req, "params", params);

        char *msg = cJSON_PrintUnformatted(req);
        lsp_send(client, msg);
        lsp_track_pending(client, init_id, lsp_on_initialize_response);

        free(msg);
        cJSON_Delete(req);

        return 0;
    }

    return -1;
}

void lspDidOpen(LspClient* lsp, struct buffer_t* buf)
{
    if (!lsp || !buf)
        return;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    char* text = bufferToText(buf);
    if (!text) return;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "textDocument/didOpen");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddStringToObject(textDoc, "languageId", lsp->language_id[0] ? lsp->language_id : "c");
    cJSON_AddNumberToObject(textDoc, "version", 1);
    cJSON_AddStringToObject(textDoc, "text", text);
    cJSON_AddItemToObject(params, "textDocument", textDoc);
    cJSON_AddItemToObject(root, "params", params);

    char* msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);

    free(text);
    free(msg);
    cJSON_Delete(root);
}

void lspDidChange(LspClient* lsp, struct buffer_t* buf)
{
    if (!lsp || !buf || lsp->state != LSP_READY)
        return;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    char* text = bufferToText(buf);
    if (!text) return;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "textDocument/didChange");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddNumberToObject(textDoc, "version", buf->dirty);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    cJSON* changes = cJSON_CreateArray();
    cJSON* change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "text", text); // full-document sync
    cJSON_AddItemToArray(changes, change);
    cJSON_AddItemToObject(params, "contentChanges", changes);

    cJSON_AddItemToObject(root, "params", params);

    char* msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);

    free(text);
    free(msg);
    cJSON_Delete(root);
}

void lspDidClose(LspClient* lsp, struct buffer_t* buf)
{
    if (!lsp || !buf)
        return;

    char uri[PATH_MAX + 8];
    char *string_fullname = (char*)utf8ToString(buf->fullname);
    snprintf(uri, sizeof(uri), "file://%s", string_fullname);
    free(string_fullname);
    string_fullname = NULL;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "textDocument/didClose");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);
    cJSON_AddItemToObject(root, "params", params);

    char* msg = cJSON_PrintUnformatted(root);
    lsp_send(lsp, msg);

    free(msg);
    cJSON_Delete(root);
}

void lspRequestCompletion(LspClient *lsp, struct buffer_t *buf,
                          int line, int col,
                          int screen_x, int screen_y)
{
    if (!lsp || lsp->state != LSP_READY) return;

    lsp->completion_cursor_x = screen_x;
    lsp->completion_cursor_y = screen_y;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    int req_id = lsp->next_id++;

    cJSON* req = cJSON_CreateObject();
    cJSON* params = cJSON_CreateObject();
    cJSON* td = cJSON_CreateObject();
    cJSON* position = cJSON_CreateObject();

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

    char* msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_completion_response);
    lsp->completion_requested = 1;
    lsp->completion_latest_request_id = req_id;

    cJSON_Delete(req);
    free(msg);
}

void lspRequestDefinition(LspClient *lsp, struct buffer_t *buf,
                          int line, int col)
{
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    int req_id = lsp->next_id++;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/definition");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    cJSON* position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);

    cJSON_AddItemToObject(req, "params", params);

    char* msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_definition_response);

    free(msg);
    cJSON_Delete(req);
}

void lspRequestHover(LspClient *lsp, struct buffer_t *buf, int line, int col)
{
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    int req_id = lsp->next_id++;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/hover");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    cJSON* position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);

    cJSON_AddItemToObject(req, "params", params);

    char* msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_hover_response);

    free(msg);
    cJSON_Delete(req);
}

void lspRequestRename(LspClient *lsp, struct buffer_t *buf, int line, int col,
                      const char *new_name)
{
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    int req_id = lsp->next_id++;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/rename");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    cJSON* position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "line", line);
    cJSON_AddNumberToObject(position, "character", col);
    cJSON_AddItemToObject(params, "position", position);
    cJSON_AddStringToObject(params, "newName", new_name);

    cJSON_AddItemToObject(req, "params", params);

    char* msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    lsp_track_pending(lsp, req_id, lsp_on_rename_response);
    lspAwaitResponse(lsp, req_id, 3000);

    free(msg);
    cJSON_Delete(req);
}

void lspRequestFormatting(LspClient *lsp, struct buffer_t *buf)
{
    if (!lsp || lsp->state != LSP_READY || !buf)
        return;

    char uri[PATH_MAX + 8];
    snprintf(uri, sizeof(uri), "file://%s", (char*) utf8ToString(buf->fullname));

    int req_id = lsp->next_id++;

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "method", "textDocument/formatting");

    cJSON* params = cJSON_CreateObject();
    cJSON* textDoc = cJSON_CreateObject();
    cJSON_AddStringToObject(textDoc, "uri", uri);
    cJSON_AddItemToObject(params, "textDocument", textDoc);

    cJSON* options = cJSON_CreateObject();
    cJSON_AddNumberToObject(options, "tabSize", E.constantes.TAB_LENGTH);
    cJSON_AddBoolToObject(options, "insertSpaces", 1);
    cJSON_AddItemToObject(params, "options", options);

    cJSON_AddItemToObject(req, "params", params);

    char* msg = cJSON_PrintUnformatted(req);
    lsp_send(lsp, msg);
    g_format_target_buf = buf;
    lsp_track_pending(lsp, req_id, lsp_on_formatting_response);
    lspAwaitResponse(lsp, req_id, 3000);

    free(msg);
    cJSON_Delete(req);
}

int lspAwaitResponse(LspClient *lsp, int request_id, int timeout_ms)
{
    if (!lsp || lsp->read_from_server_fd < 0)
        return 0;

    long deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline)
    {
        int still_pending = 0;
        for (int i = 0; i < lsp->pending_count; i++)
        {
            if (lsp->pending[i].id == request_id) { still_pending = 1; break; }
        }
        if (!still_pending)
            return 1;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(lsp->read_from_server_fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; // 50ms
        int ready = select(lsp->read_from_server_fd + 1, &fds, NULL, NULL, &tv);
        if (ready > 0)
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
    if (!E.constantes.LSP || !E.lsp_client || E.lsp_client->state != LSP_READY)
        return;

    EditorPane *active = splitScreenGetActivePane();
    struct buffer_t *buf = bufferFindById(active->buffer_id);
    if (!buf || buf->type != FILE_BUFF || !buf->b_lsp_open)
        return;

    lspDidChange(E.lsp_client, buf);
    lspRequestCompletion(E.lsp_client, buf, buf->y, buf->x,
                          active->cursor_x + active->origin_x + GUTTER_WIDTH,
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
 * @brief Called from editorRunLoop's select() when data is available on
 * lsp->read_from_server_fd. Non-blocking: drains whatever's currently
 * available and feeds it to the incremental parser.
 */
void lspPoll(LspClient *client)
{
    if (!client || client->read_from_server_fd < 0)
        return;

    char chunk[4096];
    ssize_t n;
    while ((n = read(client->read_from_server_fd, chunk, sizeof(chunk))) > 0)
        lsp_feed(client, chunk, (size_t) n);
    // n <= 0: EAGAIN (nothing more right now) or EOF/error — either way,
    // nothing further to do this cycle. EOF-triggered process-death
    // detection would need a waitpid check here, mirroring ptySessionIsAlive.
}

void lspShutdown(LspClient *client)
{
    if (!client)
        return;

    if (E.buffers != NULL)
    {
        int first_buffer_id = E.buffers->buffer_id;
        struct buffer_t* b = E.buffers;
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
                    lspDidClose(client, b);
                b->b_lsp_open = 0;
            }
            b = b->next;
        } while (b->buffer_id != first_buffer_id);
    }

    if (client->state == LSP_READY)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(root, "id", client->next_id++);
        cJSON_AddStringToObject(root, "method", "shutdown");

        char *msg = cJSON_PrintUnformatted(root);
        lsp_send(client, msg);
        free(msg);
        cJSON_Delete(root);

        client->state = LSP_SHUTTING_DOWN;

        int timeout = 1000;
        while (client->state == LSP_SHUTTING_DOWN && timeout > 0)
        {
            usleep(10000);
            timeout -= 10;
            lspPoll(client);
        }

        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "jsonrpc", "2.0");
        cJSON_AddStringToObject(root, "method", "exit");

        msg = cJSON_PrintUnformatted(root);
        lsp_send(client, msg);
        free(msg);
        cJSON_Delete(root);
    }

    client->state = LSP_SHUTDOWN;

    if (client->write_to_server_fd > 0) { close(client->write_to_server_fd); client->write_to_server_fd = -1; }
    if (client->read_from_server_fd > 0) { close(client->read_from_server_fd); client->read_from_server_fd = -1; }

    // free(client->recv_buf);
    client->recv_buf = NULL;

    if (client->pid > 0)
    {
        kill(client->pid, SIGTERM);
        usleep(100000);

        int status;
        pid_t result = waitpid(client->pid, &status, WNOHANG);
        if (result == 0)
        {
            kill(client->pid, SIGKILL);
            waitpid(client->pid, NULL, 0);
        }
        client->pid = -1;
    }
}

void lspCleanup(LspClient *client)
{
    if (!client)
        return;
    lspShutdown(client);
    free(client);
}
