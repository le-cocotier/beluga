#ifndef BELUGA_JSONRPC_STREAM_H
#define BELUGA_JSONRPC_STREAM_H

#include <stddef.h>

#include "os.h"

// ─── Content-Length framed JSON ──────────────────────────────────────────────
// The transport the Language Server Protocol and the Debug Adapter Protocol
// share: each message is "Content-Length: N\r\n\r\n" followed by N bytes of
// JSON. completion.c (LSP) and dap.c (debugger) both read and write through
// this.

typedef struct
{
    char *buf; // bytes received but not yet dispatched (a partial message)
    size_t len;
    size_t cap;
} JsonRpcStream;

// Appends `len` bytes to the stream and calls on_message(ctx, body) for
// every complete message now buffered, body NUL-terminated. A partial
// message stays buffered for the next call.
void jsonRpcStreamFeed(JsonRpcStream *s, const char *data, size_t len,
                       void (*on_message)(void *ctx, const char *body), void *ctx);

// Frees what's buffered. Never from inside on_message.
void jsonRpcStreamFree(JsonRpcStream *s);

// Writes `json` to the process's stdin with its Content-Length header.
// `tag` ("LSP", "DAP") prefixes the copy appDebug() logs.
void jsonRpcWrite(OsProc *proc, const char *json, const char *tag);

#endif // BELUGA_JSONRPC_STREAM_H
