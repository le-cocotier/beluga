/**
 * @file daemon_frame.c
 * @brief Framing for the daemon <-> client connection (see daemon_frame.h).
 */

#include "../include/daemon_frame.h"
#include "../include/galloc.h"

#include <string.h>

int frameSend(OsConn *conn, const char type, const void *payload, const size_t len) {
  unsigned char header[5];
  header[0] = (unsigned char)type;
  header[1] = (unsigned char)(len >> 24);
  header[2] = (unsigned char)(len >> 16);
  header[3] = (unsigned char)(len >> 8);
  header[4] = (unsigned char)len;
  if (os_conn_write(conn, header, sizeof(header)) < 0)
    return -1;
  if (len > 0 && os_conn_write(conn, payload, len) < 0)
    return -1;
  return 0;
}

long frameRead(FrameReader *r, OsConn *conn, const int timeout_ms) {
  unsigned char *grown;
  size_t cap;
  long n;

  // Drop what frameNext() already handed out before reading more.
  if (r->used > 0) {
    memmove(r->buf, r->buf + r->used, r->len - r->used);
    r->len -= r->used;
    r->used = 0;
  }
  if (r->cap - r->len < 4096) {
    cap = r->cap ? r->cap * 2 : 8192;
    grown = grealloc(r->buf, cap);
    if (!grown)
      return -1;
    r->buf = grown;
    r->cap = cap;
  }
  n = os_stream_read(os_conn_input(conn), r->buf + r->len, r->cap - r->len, timeout_ms);
  if (n > 0)
    r->len += (size_t)n;
  return n;
}

int frameNext(FrameReader *r, char *type, const unsigned char **payload, size_t *len) {
  const unsigned char *p = r->buf + r->used;
  const size_t avail = r->len - r->used;
  size_t n;

  if (avail < 5)
    return 0;
  n = ((size_t)p[1] << 24) | ((size_t)p[2] << 16) | ((size_t)p[3] << 8) | p[4];
  if (n > FRAME_MAX_PAYLOAD)
    return -1;
  if (avail < 5 + n)
    return 0;
  *type = (char)p[0];
  *payload = p + 5;
  *len = n;
  r->used += 5 + n;
  return 1;
}

void frameReaderFree(FrameReader *r) {
  gfree(r->buf);
  r->buf = NULL;
  r->len = r->cap = r->used = 0;
}
