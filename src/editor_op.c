#include "../include/editor_op.h"
#include "../include/async_process.h"
#include "../include/galloc.h"

#include <stdarg.h>
#include <string.h>
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
  const EditorPane *active = splitScreenGetActivePane();
  struct buffer_t *buf = bufferFindById(active->buffer_id);
  const Row *row = &buf->row[buf->y];
  int column;
  switch (key) {
  case ARROW_RIGHT:
    if (row && buf->x < row->size) {
      buf->x++;
      // Combining marks belong to the character before them: step over
      // them so the cursor never sits between a letter and its accent.
      while (buf->x < row->size && utf8CharWidth(&row->characters[buf->x]) == 0)
        buf->x++;
    } else if (row && buf->y < buf->numrows - 1) {
      buf->y++;
      buf->x = 0;
    }
    break;
  case ARROW_DOWN:
  case ARROW_UP:
    if ((key == ARROW_DOWN && buf->y < buf->numrows - 1) ||
        (key == ARROW_UP && buf->y != 0)) {
      // Keep the screen column, not the character index: with wide
      // characters on either line the two differ.
      column = utf8ColumnOf(row->characters, row->size, buf->x);
      buf->y += key == ARROW_DOWN ? 1 : -1;
      row = &buf->row[buf->y];
      buf->x = utf8IndexAtColumn(row->characters, row->size, column);
    }
    break;
  case ARROW_LEFT:
    if (buf->x != 0) {
      --buf->x;
      while (buf->x > 0 && utf8CharWidth(&row->characters[buf->x]) == 0)
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

// TODO(TODO.md): paste — bulk insert (see editorPaste in builtins.c)

// The system clipboard (os_clipboard_*), as galloc'd text the callers
// gfree(). Line endings are LF on our side whatever the platform uses.
char *editorGetClipboard(void) {
  char *text = os_clipboard_get();
  size_t len;
  char *buf;
  if (!text)
    return NULL;
  len = strlen(text);
  buf = galloc(len + 1);
  if (buf)
    memcpy(buf, text, len + 1);
  free(text);
  return buf;
}

void editorSetClipboard(const char *text, const int len) {
  if (text && len > 0)
    os_clipboard_set(text, (size_t)len);
}

/**
 * @brief Upper bound on the streams editorRunLoop() waits on: the keyboard,
 * one per terminal pane, the language server, background processes.
 */
#define RUN_LOOP_MAX_STREAMS 64
#define RUN_LOOP_MAX_TERMINALS 32

void editorRunLoop(void) {
  // Polled every tick instead of driven by SIGWINCH: nothing ever actually
  // installed a WINCH handler (editorInstallSignalHandlers(), which used
  // to live here, was never called from main()), so resizing silently did
  // nothing at all. The 50ms wait timeout below already bounds how often
  // this loop comes back around, so comparing the terminal's actual size
  // against what was last seen catches a resize within one tick —
  // dynamically, and without depending on signal delivery semantics that
  // vary across terminals and platforms (Windows has no SIGWINCH at all).
  int last_rows = 0, last_cols = 0;
  int rows;
  int cols;
  OsStream *streams[RUN_LOOP_MAX_STREAMS];
  int ready[RUN_LOOP_MAX_STREAMS];
  struct buffer_t *terminals[RUN_LOOP_MAX_TERMINALS];
  int n_streams;
  int n_terminals;
  int lsp_index;
  int first_buffer_id;
  struct buffer_t *b;
  int i;
  unsigned char chunk[4096];
  long n;
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

    // Everything there is to wait for: keys first (index 0), then each
    // terminal pane's shell, the language server, background processes.
    n_streams = 0;
    streams[n_streams++] = os_term_input();

    n_terminals = 0;
    if (E.buffers != NULL) {
      first_buffer_id = E.buffers->buffer_id;
      b = E.buffers;
      do {
        if (b->type == TERMINAL_BUFF && b->pty && n_terminals < RUN_LOOP_MAX_TERMINALS) {
          terminals[n_terminals++] = b;
          streams[n_streams++] = ptySessionOutput(b);
        }
        b = b->next;
      } while (b->buffer_id != first_buffer_id);
    }

    lsp_index = -1;
    if (E.constantes.LSP && E.lsp_client && E.lsp_client->proc) {
      lsp_index = n_streams;
      streams[n_streams++] = os_proc_output(E.lsp_client->proc);
    }

    n_streams += asyncProcessStreams(streams + n_streams, RUN_LOOP_MAX_STREAMS - n_streams);

    if (os_wait(streams, n_streams, 50, ready) > 0) { // 50ms tick
      for (i = 0; i < n_terminals; i++) {
        if (!ready[1 + i])
          continue;
        b = terminals[i];
        while ((n = os_stream_read(ptySessionOutput(b), chunk, sizeof(chunk), 0)) > 0) {
          ansiParserFeed(b, chunk, (size_t)n);
          b->x = b->term_state.cursor_x;
          b->y = b->term_state.cursor_y;
        }
        // The shell has exited: release it. The buffer keeps showing what
        // it last drew.
        if (n < 0)
          ptySessionClose(b);
      }

      if (lsp_index >= 0 && ready[lsp_index])
        lspPoll(E.lsp_client);

      // Before keys: a finished process's callback (e.g. a refresh after
      // a push) should land before the next keystroke acts on its buffer.
      asyncProcessPoll();

      if (ready[0])
        editorProcessKeypress();
    }

    // Runs every ~50ms tick regardless of what woke the wait up, so a
    // scheduled completion request fires shortly after its debounce
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
