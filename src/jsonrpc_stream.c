/**
 * @file jsonrpc_stream.c
 * @brief Content-Length framing for JSON messages, shared by the LSP client
 * (completion.c) and the debug adapter client (dap.c)
 */

#include "../include/jsonrpc_stream.h"

#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Feed newly-arrived bytes into the incremental Content-Length/JSON
 * parser. Non-blocking: only acts on complete messages; partial messages
 * persist in s->buf across calls.
 */
void jsonRpcStreamFeed(JsonRpcStream *s, const char *data, size_t len,
                       void (*on_message)(void *ctx, const char *body), void *ctx)
{
    // Grow the persistent receive buffer and append
    const size_t needed = s->len + len;
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
    if (needed > s->cap)
    {
        new_cap = s->cap ? s->cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        grown = grealloc(s->buf, new_cap);
        if (!grown) return; // best-effort: drop data rather than crash
        s->buf = grown;
        s->cap = new_cap;
    }
    if (len == 0)
        return;
    memcpy(s->buf + s->len, data, len);
    s->len += len;

    // Try to extract as many complete messages as are currently buffered
    while (1)
    {
        header_end = utilMemmem(s->buf, s->len, "\r\n\r\n", 4);
        if (!header_end) break; // headers not fully arrived yet

        header_len = (int)(header_end - s->buf);
        copy_len = header_len < (int)sizeof(header_copy) - 1 ? header_len : (int)sizeof(header_copy) - 1;
        memcpy(header_copy, s->buf, copy_len);
        header_copy[copy_len] = '\0';

        content_length = 0;
        cl = strstr(header_copy, "Content-Length: ");
        if (cl) content_length = atoi(cl + 16);

        body_start = header_len + 4;
        if (content_length <= 0)
        {
            // No usable Content-Length (missing, 0, or garbage): there is
            // no body to wait for. Drop the header and carry on, or every
            // later message would wait behind it forever.
            memmove(s->buf, s->buf + body_start, s->len - (size_t) body_start);
            s->len -= (size_t) body_start;
            continue;
        }
        if (s->len < (size_t) body_start + (size_t) content_length)
            break; // body not fully arrived yet

        // Full message available — extract and dispatch
        body = galloc(content_length + 1);
        if (body)
        {
            memcpy(body, s->buf + body_start, content_length);
            body[content_length] = '\0';
            on_message(ctx, body);
            gfree(body);
        }

        // Shift any remaining buffered bytes down to the front
        consumed = body_start + content_length;
        memmove(s->buf, s->buf + consumed, s->len - consumed);
        s->len -= consumed;
    }
}

void jsonRpcStreamFree(JsonRpcStream *s)
{
    gfree(s->buf);
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
}

void jsonRpcWrite(OsProc *proc, const char *json, const char *tag)
{
    const int body_len = (int) strlen(json);
    char header[1024];
    const int header_len = snprintf(header, sizeof(header),
                              "Content-Length: %d\r\n\r\n", body_len);

    if (!proc)
        return;
    os_proc_write(proc, header, (size_t) header_len);
    os_proc_write(proc, json, (size_t) body_len);

    appDebug("[%s →] Content-Length: %d | %s\n", tag, body_len, json);
}
