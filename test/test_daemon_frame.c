/**
 * @file test_daemon_frame.c
 * @brief Unit tests for the frames the daemon and its clients exchange
 * (daemon_frame.c): cutting a byte stream into frames, and a round trip
 * over a real connection.
 */

#include "test.h"
#include "../include/daemon_frame.h"
#include "../include/galloc.h"
#include "../include/os.h"

#include <time.h>

// Puts raw bytes into a reader, as frameRead() would.
static void push(FrameReader *r, const void *bytes, size_t n) {
  if (r->len + n > r->cap) {
    r->cap = (r->len + n) * 2;
    r->buf = grealloc(r->buf, r->cap);
  }
  memcpy(r->buf + r->len, bytes, n);
  r->len += n;
}

static void testFrameNext(void) {
  FrameReader r = {0};
  char type;
  const unsigned char *payload;
  size_t len;
  const unsigned char two[] = {'I', 0, 0, 0, 3, 'a', 'b', 'c',
                               'R', 0, 0, 0, 0};
  const unsigned char big_len[] = {'O', 0, 0, 1, 0}; // 256 bytes

  // Nothing, then a header cut short: no frame yet.
  CHECK_INT(frameNext(&r, &type, &payload, &len), 0);
  push(&r, two, 3);
  CHECK_INT(frameNext(&r, &type, &payload, &len), 0);
  push(&r, two + 3, sizeof(two) - 3);

  CHECK_INT(frameNext(&r, &type, &payload, &len), 1);
  CHECK(type == FRAME_INPUT);
  CHECK_INT(len, 3);
  CHECK(memcmp(payload, "abc", 3) == 0);

  // An empty payload is a frame too.
  CHECK_INT(frameNext(&r, &type, &payload, &len), 1);
  CHECK(type == FRAME_RESIZE);
  CHECK_INT(len, 0);
  CHECK_INT(frameNext(&r, &type, &payload, &len), 0);

  // A payload not fully arrived waits.
  push(&r, big_len, sizeof(big_len));
  CHECK_INT(frameNext(&r, &type, &payload, &len), 0);
  frameReaderFree(&r);
  CHECK(r.buf == NULL && r.len == 0 && r.used == 0);
}

static void testOversizedFrame(void) {
  FrameReader r = {0};
  char type;
  const unsigned char *payload;
  size_t len;
  // 16 MiB + 1: a broken peer.
  const unsigned char header[] = {'O', 0x01, 0x00, 0x00, 0x01};

  push(&r, header, sizeof(header));
  CHECK_INT(frameNext(&r, &type, &payload, &len), -1);
  frameReaderFree(&r);
}

// Reads from `conn` until a frame is complete, or about two seconds pass.
static int readFrame(FrameReader *r, OsConn *conn, char *type,
                     const unsigned char **payload, size_t *len) {
  int tries;
  int got;
  for (tries = 0; tries < 20; tries++) {
    got = frameNext(r, type, payload, len);
    if (got != 0)
      return got;
    if (frameRead(r, conn, 100) < 0)
      return -1;
  }
  return 0;
}

static void testRoundTrip(void) {
  char name[64];
  char err[256] = "";
  OsServer *server;
  OsConn *client;
  OsConn *daemon = NULL;
  OsStream *listening;
  FrameReader r = {0};
  char type;
  const unsigned char *payload;
  size_t len;
  int ready;
  int tries;
  unsigned char *big;
  size_t big_len;
  size_t i;
  int ok;
  const char *hello = DAEMON_PROTOCOL "\n24 80\n/\n";

  snprintf(name, sizeof(name), "test-frame-%ld-%ld", os_getpid(), (long)time(NULL));
  server = os_server_listen(name, err, sizeof(err));
  if (!server) {
    fprintf(stderr, "os_server_listen: %s\n", err);
    test_failures++;
    return;
  }
  client = os_conn_connect(name, err, sizeof(err));
  CHECK(client != NULL);
  listening = os_server_stream(server);
  for (tries = 0; client && !daemon && tries < 20; tries++)
    if (os_wait(&listening, 1, 100, &ready) > 0)
      daemon = os_server_accept(server);
  CHECK(daemon != NULL);
  if (!client || !daemon)
    goto done;

  CHECK_INT(frameSend(client, FRAME_HELLO, hello, strlen(hello)), 0);
  CHECK_INT(frameSend(client, FRAME_KILL, "", 0), 0);
  CHECK_INT(readFrame(&r, daemon, &type, &payload, &len), 1);
  CHECK(type == FRAME_HELLO);
  CHECK_INT(len, strlen(hello));
  CHECK(len == strlen(hello) && memcmp(payload, hello, len) == 0);
  CHECK_INT(readFrame(&r, daemon, &type, &payload, &len), 1);
  CHECK(type == FRAME_KILL);
  CHECK_INT(len, 0);

  // A larger binary frame, the other way. (Kept under 8 KiB: nothing
  // reads while it's sent, and macOS socket buffers are that small.)
  big_len = 6000;
  big = galloc(big_len);
  for (i = 0; i < big_len; i++)
    big[i] = (unsigned char)(i * 7);
  frameReaderFree(&r);
  CHECK_INT(frameSend(daemon, FRAME_OUTPUT, big, big_len), 0);
  CHECK_INT(readFrame(&r, client, &type, &payload, &len), 1);
  CHECK(type == FRAME_OUTPUT);
  CHECK_INT(len, big_len);
  ok = len == big_len && memcmp(payload, big, big_len) == 0;
  CHECK(ok);
  gfree(big);

  // The other end closing ends the stream.
  os_conn_close(daemon);
  daemon = NULL;
  CHECK_INT(readFrame(&r, client, &type, &payload, &len), -1);

done:
  frameReaderFree(&r);
  if (daemon)
    os_conn_close(daemon);
  if (client)
    os_conn_close(client);
  os_server_close(server);
}

int main(void) {
  testInit();
  testFrameNext();
  testOversizedFrame();
  testRoundTrip();
  return testDone();
}
