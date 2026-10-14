/**
 * @file fuzz_lsp.c
 * @brief Fuzz target: arbitrary bytes from a language server into the
 * client's framing and message handling (completion.c)
 * @details Covers the Content-Length framing, cJSON, notifications
 * (publishDiagnostics) and requests from the server (workspace/applyEdit,
 * ...). The client has no process, so replies go nowhere. The input is
 * fed in two reads, split at a point chosen by its first byte.
 *
 * One file buffer is open, /fuzz.c (URI file:///fuzz.c), for edits to
 * land in. An edit to any other file would open it, which needs the
 * whole editor (Lisp hooks, git, panes), so inputs that could name
 * another file are skipped: any "file:" not followed by "///fuzz.c", and
 * any JSON escape that could spell one.
 */

#include "../../include/completion.h"
#include "../../include/galloc.h"
#include "../test.h"

#include "../../include/buffer.h"
#include "../../include/utf8.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int namesOnlyFuzzFile(const uint8_t *data, size_t size) {
  size_t i;
  for (i = 0; i + 1 < size; i++) {
    if (data[i] == '\\' && (data[i + 1] == '/' || data[i + 1] == 'u'))
      return 0;
    if (i + 5 <= size && memcmp(data + i, "file:", 5) == 0 &&
        (i + 14 > size || memcmp(data + i + 5, "///fuzz.c", 9) != 0))
      return 0;
  }
  return 1;
}

static Buffer *openFuzzFile(void) {
  Buffer *buf;
  buf = testBufferFromText("int main(void) {\n  return 0;\n}");
  buf->fullname = stringToUtf8((unsigned char *)"/fuzz.c");
  buf->buffer_id = 0;
  buf->next = buf;
  buf->prev = buf;
  return buf;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  LspClient *lsp;
  size_t split;
  Buffer *buf;
  static int initialized = 0;
  if (!initialized) {
    testInit();
    initialized = 1;
  }
  if (size == 0 || !namesOnlyFuzzFile(data, size))
    return 0;
  buf = openFuzzFile();
  E.buffers = buf;
  lsp = gcalloc(1, sizeof(LspClient));
  lsp->state = LSP_READY;
  E.lsp_clients[0] = lsp;
  E.number_of_lsp_clients = 1;
  split = size > 1 ? (size_t)data[0] % size : 0;
  if (split == 0)
    split = 1;
  lspFeed(lsp, (const char *)data + 1, split - 1);
  lspFeed(lsp, (const char *)data + split, size - split);
  E.lsp_clients[0] = NULL;
  E.number_of_lsp_clients = 0;
  E.buffers = NULL;
  bufferFree(buf);
  gfree(lsp->recv_buf);
  gfree(lsp);
  return 0;
}
