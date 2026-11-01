/**
 * @file test_jsonrpc_stream.c
 * @brief Unit tests for the Content-Length framing the LSP and DAP clients
 * read through (jsonrpc_stream.c).
 */

#include "test.h"
#include "../include/galloc.h"
#include "../include/jsonrpc_stream.h"
#include "../include/utils.h"

#define MAX_MESSAGES 8

typedef struct {
  char *bodies[MAX_MESSAGES];
  int count;
} Received;

static void onMessage(void *ctx, const char *body) {
  Received *r = ctx;
  if (r->count < MAX_MESSAGES)
    r->bodies[r->count] = utilStrdup(body);
  r->count++;
}

static void resetReceived(Received *r) {
  int i;
  for (i = 0; i < r->count && i < MAX_MESSAGES; i++)
    gfree(r->bodies[i]);
  memset(r, 0, sizeof(*r));
}

static void feed(JsonRpcStream *s, Received *r, const char *data) {
  jsonRpcStreamFeed(s, data, strlen(data), onMessage, r);
}

static void testWholeMessage(void) {
  JsonRpcStream s = {0};
  Received r = {0};

  feed(&s, &r, "Content-Length: 7\r\n\r\n{\"a\":1}");
  CHECK_INT(r.count, 1);
  CHECK_STR(r.bodies[0], "{\"a\":1}");
  CHECK_INT(s.len, 0);

  resetReceived(&r);
  jsonRpcStreamFree(&s);
}

static void testSplitAnywhere(void) {
  const char *msg = "Content-Length: 12\r\nContent-Type: x\r\n\r\n{\"id\":12345}";
  const size_t total = strlen(msg);
  JsonRpcStream s = {0};
  Received r = {0};
  size_t cut;

  // Every split point, in the header, at the blank line and in the body,
  // gives the same one message once the rest arrives.
  for (cut = 0; cut <= total; cut++) {
    jsonRpcStreamFeed(&s, msg, cut, onMessage, &r);
    if (cut < total)
      CHECK_INT(r.count, 0);
    jsonRpcStreamFeed(&s, msg + cut, total - cut, onMessage, &r);
    CHECK_INT(r.count, 1);
    CHECK_STR(r.bodies[0], "{\"id\":12345}");
    CHECK_INT(s.len, 0);
    resetReceived(&r);
  }

  // One byte at a time.
  for (cut = 0; cut < total; cut++)
    jsonRpcStreamFeed(&s, msg + cut, 1, onMessage, &r);
  CHECK_INT(r.count, 1);
  CHECK_STR(r.bodies[0], "{\"id\":12345}");

  resetReceived(&r);
  jsonRpcStreamFree(&s);
}

static void testSeveralInOneChunk(void) {
  JsonRpcStream s = {0};
  Received r = {0};

  feed(&s, &r, "Content-Length: 1\r\n\r\n1Content-Length: 2\r\n\r\n22Content-Length: 3\r\n\r\n33");
  CHECK_INT(r.count, 2);
  CHECK_STR(r.bodies[0], "1");
  CHECK_STR(r.bodies[1], "22");
  // The third waits for its last byte.
  feed(&s, &r, "3");
  CHECK_INT(r.count, 3);
  CHECK_STR(r.bodies[2], "333");

  resetReceived(&r);
  jsonRpcStreamFree(&s);
}

static void testBadHeaders(void) {
  JsonRpcStream s = {0};
  Received r = {0};

  // A header without a usable length is dropped, and doesn't hold up the
  // messages behind it.
  feed(&s, &r, "Content-Type: x\r\n\r\n");
  feed(&s, &r, "Content-Length: 0\r\n\r\n");
  feed(&s, &r, "Content-Length: junk\r\n\r\n");
  feed(&s, &r, "Content-Length: 2\r\n\r\nok");
  CHECK_INT(r.count, 1);
  CHECK_STR(r.bodies[0], "ok");
  CHECK_INT(s.len, 0);

  resetReceived(&r);
  jsonRpcStreamFree(&s);
}

static void testBodyWithNulAndUtf8(void) {
  JsonRpcStream s = {0};
  Received r = {0};
  // Content-Length counts bytes: "é" is two of them.
  const char msg[] = "Content-Length: 4\r\n\r\n\"\xc3\xa9\"";

  jsonRpcStreamFeed(&s, msg, sizeof(msg) - 1, onMessage, &r);
  CHECK_INT(r.count, 1);
  CHECK_STR(r.bodies[0], "\"\xc3\xa9\"");

  resetReceived(&r);
  jsonRpcStreamFree(&s);
}

static void testLargeBody(void) {
  JsonRpcStream s = {0};
  Received r = {0};
  char header[64];
  char *body;
  const size_t n = 100000;

  body = galloc(n + 1);
  memset(body, 'x', n);
  body[n] = '\0';
  snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", n);
  feed(&s, &r, header);
  // Arrives in pieces bigger than the stream's first allocation.
  jsonRpcStreamFeed(&s, body, 5000, onMessage, &r);
  CHECK_INT(r.count, 0);
  jsonRpcStreamFeed(&s, body + 5000, n - 5000, onMessage, &r);
  CHECK_INT(r.count, 1);
  CHECK(r.bodies[0] && strlen(r.bodies[0]) == n);

  gfree(body);
  resetReceived(&r);
  jsonRpcStreamFree(&s);
}

int main(void) {
  testInit();
  testWholeMessage();
  testSplitAnywhere();
  testSeveralInOneChunk();
  testBadHeaders();
  testBodyWithNulAndUtf8();
  testLargeBody();
  return testDone();
}
