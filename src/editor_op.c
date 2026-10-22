#include "../include/editor_op.h"
#include "../include/autosave.h"
#include "../include/file_io.h"
#include "../include/async_process.h"
#include "../include/galloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/ansi_parser.h"
#include "../include/buffer.h"
#include "../include/completion.h"
#include "../include/data.h"
#include "../include/input.h"
#include "../include/output.h"
#include "../include/pty_session.h"
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
  char *msg;
  va_start(ap, fmt);
  utf8VaPrintf(E.status_msg, PATH_MAX, fmt, ap);
  va_end(ap);
  E.status_msg_time = time(NULL);
  // No screen in batch mode (beluga --sync-packages): print it instead.
  if (E.batch && *E.status_msg[0].character) {
    msg = (char *)utf8ToString(E.status_msg);
    if (msg) {
      printf("%s\n", msg);
      fflush(stdout);
      gfree(msg);
    }
  }
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

// The system clipboard (os_clipboard_*), as galloc'd text the callers
// gfree(). Line endings are LF on our side whatever the platform uses.
char *editorGetClipboard(void) {
  return os_clipboard_get();
}

void editorSetClipboard(const char *text, const int len) {
  if (text && len > 0)
    os_clipboard_set(text, (size_t)len);
}

void editorWatchStreams(RunLoopWatch *w, OsStream **streams, int *n_streams,
                        const int max) {
  int first_buffer_id;
  struct buffer_t *b;
  int i;

  w->n_terminals = 0;
  w->first_terminal = *n_streams;
  if (E.buffers != NULL) {
    first_buffer_id = E.buffers->buffer_id;
    b = E.buffers;
    do {
      if (b->type == TERMINAL_BUFF && b->pty &&
          w->n_terminals < RUN_LOOP_MAX_TERMINALS && *n_streams < max) {
        w->terminals[w->n_terminals++] = b;
        streams[(*n_streams)++] = ptySessionOutput(b);
      }
      b = b->next;
    } while (b->buffer_id != first_buffer_id);
  }

  for (i = 0; i < E.number_of_lsp_clients; i++) {
    w->lsp_index[i] = -1;
    if (E.constantes.LSP && E.lsp_clients[i]->proc && *n_streams < max) {
      w->lsp_index[i] = *n_streams;
      streams[(*n_streams)++] = os_proc_output(E.lsp_clients[i]->proc);
    }
  }

  *n_streams += asyncProcessStreams(streams + *n_streams, max - *n_streams);
}

void editorHandleStreams(const RunLoopWatch *w, const int *ready) {
  struct buffer_t *b;
  unsigned char chunk[4096];
  long n;
  int i;

  for (i = 0; i < w->n_terminals; i++) {
    if (!ready[w->first_terminal + i])
      continue;
    b = w->terminals[i];
    while ((n = os_stream_read(ptySessionOutput(b), chunk, sizeof(chunk), 0)) > 0) {
      ansiParserFeed(b, chunk, (size_t)n);
      b->x = ansiParserCursorIndex(b);
      b->y = b->term_state.cursor_y;
    }
    // The shell has exited: release it. The buffer keeps showing what
    // it last drew.
    if (n < 0)
      ptySessionClose(b);
  }

  // number_of_lsp_clients can't have changed since the streams were
  // collected: clients are only added from key handling, after this.
  for (i = 0; i < E.number_of_lsp_clients; i++)
    if (w->lsp_index[i] >= 0 && ready[w->lsp_index[i]])
      lspPoll(E.lsp_clients[i]);

  // Before keys: a finished process's callback (e.g. a refresh after
  // a push) should land before the next keystroke acts on its buffer.
  asyncProcessPoll();
}

void editorTick(void) {
  // Runs every ~50ms tick regardless of what woke the wait up, so a
  // scheduled completion request fires shortly after its debounce
  // deadline passes even if the user has stopped typing entirely.
  if (E.constantes.LSP) {
    lspCompletionDebounceTick();
    lspSyncDebounceTick();
    lspDecorationsTick();
  }
  autosaveTick();
  fileCheckDiskChanges();
}

void editorCloseTerminals(void) {
  int first_buffer_id;
  struct buffer_t *b;
  if (E.buffers == NULL)
    return;
  first_buffer_id = E.buffers->buffer_id;
  b = E.buffers;
  do {
    if (b->type == TERMINAL_BUFF)
      ptySessionClose(b);
    b = b->next;
  } while (b->buffer_id != first_buffer_id);
}

void editorApplyScreenSize(const int rows, const int cols) {
  const int old_rows = E.screenrows;
  const int old_cols = E.screencols;
  E.screenrows = rows - 2;
  E.screencols = cols;
  if (E.screenrows < 1)
    E.screenrows = 1;
  if (E.screencols < 1)
    E.screencols = 1;
  splitScreenResize(old_rows, old_cols);
  // Every pane's terminal, not just the active one: a terminal sitting in
  // the other half of a split is just as affected by the window changing
  // size, and used to keep the size it was spawned with for the rest of
  // the session.
  splitScreenSyncTerminalSizes();
}

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
  RunLoopWatch watch;
  int n_streams;
  getWindowSize(&last_rows, &last_cols);

  while (!E.exit_requested) {
    if (getWindowSize(&rows, &cols) == 0 &&
        (rows != last_rows || cols != last_cols)) {
      last_rows = rows;
      last_cols = cols;
      editorApplyScreenSize(rows, cols);
    }

    // Everything there is to wait for: keys first (index 0), then each
    // terminal pane's shell, the language server, background processes.
    n_streams = 0;
    streams[n_streams++] = os_term_input();
    editorWatchStreams(&watch, streams, &n_streams, RUN_LOOP_MAX_STREAMS);

    if (os_wait(streams, n_streams, 50, ready) > 0) { // 50ms tick
      editorHandleStreams(&watch, ready);
      if (ready[0])
        editorProcessKeypress();
    }

    editorTick();

    // A keypress handled above may have requested the exit; drawing one
    // more frame here would just repaint the UI we're about to leave.
    if (E.exit_requested)
      break;

    editorRefreshScreen();
  }

  // Clean shutdown: close any live shells before exiting
  editorCloseTerminals();
  terminalRestore();
}
