#include "../include/editor_op.h"
#include "../include/async_process.h"
#include "../include/galloc.h"

#include <stdarg.h>
#include <stdlib.h>

#include "../include/ansi_parser.h"
#include "../include/buffer.h"
#include "../include/completion.h"
#include "../include/data.h"
#include "../include/input.h"
#include "../include/output.h"
#include "../include/pty_session.h"
#include "../include/row_op.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include <errno.h>
#include <sys/select.h>

extern EditorConfig E;

/**
 * @brief Sets a temporary status message for display
 * @details Formats and stores a message that will be displayed in the message
 * bar for 5 seconds. Uses printf-style variable argument formatting.
 * @param fmt Printf-style format string
 * @param ... Variable arguments for format string
 * @note Updates global editor state E (status_msg, status_msg_time)
 * @see editorDrawMessageBar()
 */
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  utf8VaPrintf(E.status_msg, PATH_MAX, fmt, ap);
  va_end(ap);
  E.status_msg_time = time(NULL);
}

/**
 * @brief Moves the cursor based on arrow key input
 * @details Updates cursor position (E.cursor_x, E.cursor_y) based on the given
 * key direction. Handles line wrapping and boundary conditions. Prevents cursor
 * from exceeding line lengths.
 * @param key The arrow key code (ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT)
 * @return 1 if cursor movement was valid, 0 if cursor was constrained to line
 * boundary
 * @note Updates global editor state E
 */
int editorMoveCursor(const int key) {
  // TODO(TODO.md): display width of wide / combining characters
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  const Row *row = &buf->row[buf->y];
  switch (key) {
  case ARROW_RIGHT:
    if (row && buf->x < row->size) {
      buf->x++;
    } else if (row && buf->y < buf->numrows - 1) {
      buf->y++;
      buf->x = 0;
    }
    break;
  case ARROW_DOWN:
    if (buf->y < buf->numrows - 1) {

      buf->y++;
      buf->x = buf->row[buf->y].size < buf->x ? buf->row[buf->y].size : buf->x;
    }
    break;
  case ARROW_UP:
    if (buf->y != 0) {
      --buf->y;
      buf->x = buf->row[buf->y].size < buf->x ? buf->row[buf->y].size : buf->x;
    }
    break;
  case ARROW_LEFT:
    if (buf->x != 0) {
      --buf->x;
    } else if (buf->y > 0) {
      --buf->y;
      buf->x = buf->row[buf->y].size;
    }
    break;
  default:
    break;
  }
  return 1;
}

// TODO(TODO.md): paste — CRLF, bulk insert (see editorPaste in builtins.c)

char *editorGetClipboard(void) {
  FILE *pipe = popen(CLIPBOARD_PASTE_CMD, "r");
  size_t cap;
  size_t len;
  char *buf;
  int c;
  char *new_buf;
  if (!pipe)
    return NULL;

  cap = 4096;
  len = 0;
  buf = galloc(cap);
  if (!buf) {
    pclose(pipe);
    return NULL;
  }

  while ((c = fgetc(pipe)) != EOF) {
    if (len + 1 >= cap) {
      cap *= 2;
      new_buf = grealloc(buf, cap);
      if (!new_buf) {
        gfree(buf);
        pclose(pipe);
        return NULL;
      }
      buf = new_buf;
    }
    buf[len++] = (char)c;
  }
  buf[len] = '\0';
  pclose(pipe);
  return buf;
}

void editorSetClipboard(const char *text, const int len) {
  FILE *pipe = popen(CLIPBOARD_COPY_CMD, "w");
  if (!pipe)
    return;
  fwrite(text, 1, len, pipe);
  pclose(pipe);
}

void editorRunLoop(void) {
  // Polled every tick instead of driven by SIGWINCH: nothing ever actually
  // installed a WINCH handler (editorInstallSignalHandlers(), which used
  // to live here, was never called from main()), so resizing silently did
  // nothing at all. The 50ms select() timeout below already bounds how
  // often this loop comes back around, so comparing the terminal's actual
  // size against what was last seen catches a resize within one tick —
  // dynamically, and without depending on signal delivery/interruption
  // semantics that vary across terminals and platforms.
  int last_rows = 0, last_cols = 0;
  int rows;
  int cols;
  fd_set readfds;
  int maxfd;
  int first_buffer_id;
  struct buffer_t *b;
  struct timeval tv;
  int ready;
  unsigned char chunk[4096];
  ssize_t n;
  getWindowSize(&last_rows, &last_cols);

  while (!E.exit_requested) {
    if (getWindowSize(&rows, &cols) == 0 &&
        (rows != last_rows || cols != last_cols)) {
      last_rows = rows;
      last_cols = cols;
      E.screenrows = rows - 2;
      E.screencols = cols;
      // Every pane's terminal, not just the active one: a terminal
      // sitting in the other half of a split is just as affected by
      // the window changing size, and used to keep the size it was
      // spawned with for the rest of the session.
      splitScreenSyncTerminalSizes();
    }

    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    maxfd = STDIN_FILENO;

    if (E.buffers != NULL) {
      first_buffer_id = E.buffers->buffer_id;
      b = E.buffers;
      do {
        if (b->type == TERMINAL_BUFF && b->pty_master_fd >= 0) {
          FD_SET(b->pty_master_fd, &readfds);
          if (b->pty_master_fd > maxfd)
            maxfd = b->pty_master_fd;
        }
        b = b->next;
      } while (b->buffer_id != first_buffer_id);
    }

    if (E.constantes.LSP && E.lsp_client &&
        E.lsp_client->read_from_server_fd >= 0) {
      FD_SET(E.lsp_client->read_from_server_fd, &readfds);
      if (E.lsp_client->read_from_server_fd > maxfd)
        maxfd = E.lsp_client->read_from_server_fd;
    }

    maxfd = asyncProcessAddFds(&readfds, maxfd);

    tv = (struct timeval){.tv_sec = 0, .tv_usec = 50000}; // 50ms tick
    ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);

    if (ready < 0) {
      if (errno == EINTR)
        continue; // interrupted by a signal — just retry
      die("select");
    }

    if (ready > 0) {
      if (E.buffers != NULL) {
        first_buffer_id = E.buffers->buffer_id;
        b = E.buffers;
        do {
          if (b->type == TERMINAL_BUFF && b->pty_master_fd >= 0 &&
              FD_ISSET(b->pty_master_fd, &readfds)) {
            while ((n = read(b->pty_master_fd, chunk, sizeof(chunk))) > 0) {
              ansiParserFeed(b, chunk, (size_t)n);
              b->x = b->term_state.cursor_x;
              b->y = b->term_state.cursor_y;
            }

            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
              ptySessionIsAlive(b); // reap if the shell exited
          }
          b = b->next;
        } while (b->buffer_id != first_buffer_id);
      }

      if (E.constantes.LSP && E.lsp_client &&
          (E.lsp_client->read_from_server_fd >= 0) &&
          FD_ISSET(E.lsp_client->read_from_server_fd, &readfds))
        lspPoll(E.lsp_client);

      // Before keys: a finished process's callback (e.g. a refresh after
      // a push) should land before the next keystroke acts on its buffer.
      asyncProcessPoll(&readfds);

      if (FD_ISSET(STDIN_FILENO, &readfds))
        editorProcessKeypress();
    }

    // Runs every ~50ms tick regardless of what select() woke up for, so
    // a scheduled completion request fires shortly after its debounce
    // deadline passes even if the user has stopped typing entirely.
    if (E.constantes.LSP) {
      lspCompletionDebounceTick();
      lspSyncDebounceTick();
    }

    // A keypress handled above may have requested the exit; drawing one
    // more frame here would just repaint the UI we're about to leave.
    if (E.exit_requested)
      break;

    editorRefreshScreen();
  }

  // Clean shutdown: close any live shells before exiting
  if (E.buffers != NULL) {
    first_buffer_id = E.buffers->buffer_id;
    b = E.buffers;
    do {
      if (b->type == TERMINAL_BUFF)
        ptySessionClose(b);
      b = b->next;
    } while (b->buffer_id != first_buffer_id);
  }
  terminalRestore();
}
