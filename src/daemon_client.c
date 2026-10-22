/**
 * @file daemon_client.c
 * @brief `beluga -c`: a window on the beluga daemon.
 * @details A thin terminal proxy. It puts the terminal in raw mode, sends
 * the keys as they come and the size when it changes, and writes what the
 * daemon draws. Everything else (buffers, Lisp, the layout of this window)
 * lives in the daemon, so the client needs none of the editor's state.
 */

#include "../include/daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/daemon_frame.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/terminal.h"

/**
 * @brief The hello frame's payload (see daemon_frame.h), galloc'd.
 */
static char *buildHello(const int rows, const int cols, char *const *files,
                        const int n_files, size_t *len) {
  char *cwd = os_getcwd();
  char *hello;
  size_t size;
  size_t used;
  int i;

  size = strlen(DAEMON_PROTOCOL) + 32 + (cwd ? strlen(cwd) : 0) + 2;
  for (i = 0; i < n_files; i++)
    size += strlen(files[i]) + 1;
  hello = galloc(size);
  if (!hello) {
    gfree(cwd);
    return NULL;
  }
  used = (size_t)snprintf(hello, size, "%s\n%d %d\n%s\n", DAEMON_PROTOCOL, rows, cols,
                          cwd ? cwd : "");
  for (i = 0; i < n_files; i++)
    used += (size_t)snprintf(hello + used, size - used, "%s\n", files[i]);
  gfree(cwd);
  *len = used;
  return hello;
}

int clientMain(const char *name, char *const *files, const int n_files) {
  char err[512];
  OsConn *conn;
  FrameReader reader = {NULL, 0, 0, 0};
  OsStream *streams[2];
  int ready[2];
  unsigned char keys[4096];
  char size_text[32];
  char *hello;
  char *message = NULL;
  size_t hello_len;
  const unsigned char *payload;
  size_t len;
  char type;
  int rows = 24, cols = 80;
  int new_rows, new_cols;
  int done = 0;
  int lost = 0;
  long n;
  int r;

  conn = os_conn_connect(name, err, sizeof(err));
  if (!conn) {
    if (strcmp(name, DAEMON_DEFAULT_NAME) == 0)
      fprintf(stderr, "beluga: %s\nStart one with: beluga --daemon\n", err);
    else
      fprintf(stderr, "beluga: %s\nStart one with: beluga --daemon=%s\n", err, name);
    return 1;
  }
  if (os_term_enter_raw() == -1) {
    fprintf(stderr, "beluga: this terminal can't be put in raw mode\n");
    os_conn_close(conn);
    return 1;
  }
  if (os_term_size(&rows, &cols) == -1 || rows < 3 || cols < 1) {
    rows = 24;
    cols = 80;
  }

  hello = buildHello(rows, cols, files, n_files, &hello_len);
  if (!hello || frameSend(conn, FRAME_HELLO, hello, hello_len) == -1)
    lost = done = 1;
  gfree(hello);
  os_term_write("\x1b[2J", 4);

  while (!done) {
    streams[0] = os_term_input();
    streams[1] = os_conn_input(conn);
    if (os_wait(streams, 2, 50, ready) > 0) {
      if (ready[0]) {
        n = os_stream_read(streams[0], keys, sizeof(keys), 0);
        if (n > 0 && frameSend(conn, FRAME_INPUT, keys, (size_t)n) == -1)
          lost = done = 1;
      }
      if (ready[1] && !done) {
        if (frameRead(&reader, conn, 0) < 0)
          lost = done = 1; // read what came before the end, below
        while ((r = frameNext(&reader, &type, &payload, &len)) == 1) {
          if (type == FRAME_OUTPUT)
            os_term_write(payload, len);
          else if (type == FRAME_EXIT) {
            message = galloc(len + 1);
            if (message) {
              memcpy(message, payload, len);
              message[len] = '\0';
            }
            lost = 0;
            done = 1;
            break;
          }
        }
        if (r < 0)
          lost = done = 1;
      }
    }

    // Polled, like the standalone editor does (Windows has no SIGWINCH).
    if (!done && os_term_size(&new_rows, &new_cols) == 0 &&
        (new_rows != rows || new_cols != cols) && new_rows >= 3 && new_cols >= 1) {
      rows = new_rows;
      cols = new_cols;
      snprintf(size_text, sizeof(size_text), "%d %d", rows, cols);
      if (frameSend(conn, FRAME_RESIZE, size_text, strlen(size_text)) == -1)
        lost = done = 1;
    }
  }

  terminalRestore();
  if (lost)
    fprintf(stderr, "beluga: lost the connection to the daemon\n");
  else if (message && message[0])
    fprintf(stderr, "%s\n", message);
  gfree(message);
  frameReaderFree(&reader);
  os_conn_close(conn);
  return lost ? 1 : 0;
}

int clientKill(const char *name, const int force) {
  char err[512];
  OsConn *conn;
  FrameReader reader = {NULL, 0, 0, 0};
  const unsigned char *payload;
  size_t len;
  char type;
  const long long deadline = os_monotonic_ms() + 10000;
  int rc = 1;

  conn = os_conn_connect(name, err, sizeof(err));
  if (!conn) {
    fprintf(stderr, "beluga: %s\n", err);
    return 1;
  }
  if (frameSend(conn, FRAME_KILL, force ? "force" : "", force ? 5 : 0) == 0) {
    while (os_monotonic_ms() < deadline) {
      if (frameRead(&reader, conn, 100) < 0 && reader.len == reader.used)
        break;
      if (frameNext(&reader, &type, &payload, &len) == 1 && type == FRAME_EXIT) {
        if (len > 0)
          fprintf(stderr, "%.*s\n", (int)len, (const char *)payload);
        rc = len > 0 ? 1 : 0;
        break;
      }
    }
  }
  frameReaderFree(&reader);
  os_conn_close(conn);
  return rc;
}
