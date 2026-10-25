/**
 * @file daemon.c
 * @brief The beluga daemon: one editor, several client windows.
 * @details
 * Shared by every window: the buffers (text, cursor, undo, mark), the Lisp
 * interpreter and everything init.lisp set up, the language servers, the
 * terminal panes and background processes.
 *
 * Per window (a ClientView): the screen size, the pane layout, the pending
 * key prefix (and which-key's timer for it), the popups, the status
 * message, the quit counter and the last frame drawn. The working
 * directory is the client's (E.cwd).
 *
 * The editor reads all of that from the global E, so rather than threading
 * a client through every function, the daemon swaps a window's view into E
 * before acting for it (clientSwitchTo()) and saves it back after. Only one
 * window is current at a time; terminal.c's screen driver sends drawing and
 * key reads to that one.
 *
 * Each 50 ms tick: accept new clients, read what clients sent, handle each
 * window's keys with that window current, then draw every window. An edit
 * in one window shows in the others on the next tick.
 *
 * A prompt (editorPrompt(): open-file, M-x, search, ...) is a nested loop
 * that reads keys until ENTER; while one window has a prompt open, the
 * others wait (emacs has the same rule for the minibuffer across
 * terminals). If that window disconnects, its prompt reads ESC and ends.
 */

#include "../include/daemon.h"

#include <stdio.h>
#include <string.h>

#include "../include/autosave.h"
#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/config_load.h"
#include "../include/daemon_frame.h"
#include "../include/dap.h"
#include "../include/data.h"
#include "../include/define.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/init.h"
#include "../include/input.h"
#include "../include/os.h"
#include "../include/output.h"
#include "../include/project.h"
#include "../include/screen_diff.h"
#include "../include/settings.h"
#include "../include/split_screen.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../include/utils.h"

extern EditorConfig E;

// Clients beyond this are turned away: the run loop waits on one stream per
// client inside RUN_LOOP_MAX_STREAMS.
#define DAEMON_MAX_CLIENTS 16

// A second Ctrl-C this soon after the first stops a daemon with unsaved
// buffers anyway.
#define DAEMON_INTERRUPT_AGAIN_MS 3000

/**
 * @brief The part of E that belongs to one window.
 * @details `cache` holds the window's last frame while another window is
 * current; while this one is current, the live cache is in output.c and
 * this field is empty.
 */
typedef struct
{
    int fresh; // never shown yet: E gets a new layout on first switch
    int screenrows, screencols;
    int cursor_x, cursor_y;
    ScreenLayout layout;
    CompletionPopup lsp_completion;
    LspInfoPopup lsp_info;
    BufferSwitcherPopup buffer_switcher;
    ListPopup list_popup;
    FileFinder file_finder;
    Utf8Char *status_msg;
    time_t status_msg_time;
    int prefix_state;
    Utf8Char *prompt_input;
    int prompt_is_command;
    int quit_times_buffer;
    int which_key_prefix;
    long which_key_since;
    // The window's current project: from its active pane's buffer, or
    // picked with project-switch (project.c).
    int current_project;
    struct const_t constantes;
    int project_last_buffer;
    ScreenCache cache;
} ClientView;

typedef struct daemon_client_t
{
    OsConn *conn;
    FrameReader reader;
    int id;            // > 0, never reused: debug sessions name their window by it
    int greeted;       // the hello arrived and the window is open
    int hello_pending; // the hello arrived; the window opens next tick
    int gone;          // the connection has ended
    int closing;       // the window is to be closed (quit, or gone)
    int rows, cols;    // the client terminal's full size
    int size_changed;  // rows/cols not applied to the view yet
    char cwd[OS_PATH_MAX];
    char **files;      // from the hello, opened with the window
    int n_files;

    // Key bytes received and not yet read by the editor.
    unsigned char *keys;
    size_t keys_head, keys_len, keys_cap;

    ClientView view;
    struct daemon_client_t *next;
} DaemonClient;

static OsServer *s_server = NULL;
static DaemonClient *s_clients = NULL;
static DaemonClient *s_current = NULL; // whose view is in E (NULL: s_idle's)
static DaemonClient *s_focused = NULL; // the window that last sent keys
static ClientView s_idle;              // E's view while no window is current
static int s_running = 0;
static int s_stop = 0; // beluga --kill was accepted
static int s_next_id = 1;
static DaemonClient *s_entered_from = NULL; // windowLeave() goes back there

/***********************************************************************************************************************
 *
 *                                                 Per-window views
 *
 **********************************************************************************************************************/

static void viewSave(ClientView *v) {
  v->screenrows = E.screenrows;
  v->screencols = E.screencols;
  v->cursor_x = E.cursor_x;
  v->cursor_y = E.cursor_y;
  v->layout = E.layout;
  v->lsp_completion = E.lsp_completion;
  v->lsp_info = E.lsp_info;
  v->buffer_switcher = E.buffer_switcher;
  v->list_popup = E.list_popup;
  v->file_finder = E.file_finder;
  v->status_msg = E.status_msg;
  v->status_msg_time = E.status_msg_time;
  v->prefix_state = E.prefix_state;
  v->prompt_input = E.prompt_input;
  v->prompt_is_command = E.prompt_is_command;
  v->quit_times_buffer = E.quit_times_buffer;
  v->which_key_prefix = E.which_key_prefix;
  v->which_key_since = E.which_key_since;
  v->current_project = E.current_project;
  v->constantes = E.constantes;
  projectSwapLastBuffer(&v->project_last_buffer);
  editorSwapScreenCache(&v->cache);
}

static void viewLoad(ClientView *v) {
  E.screenrows = v->screenrows;
  E.screencols = v->screencols;
  E.cursor_x = v->cursor_x;
  E.cursor_y = v->cursor_y;
  E.layout = v->layout;
  E.lsp_completion = v->lsp_completion;
  E.lsp_info = v->lsp_info;
  E.buffer_switcher = v->buffer_switcher;
  E.list_popup = v->list_popup;
  E.file_finder = v->file_finder;
  E.status_msg = v->status_msg;
  E.status_msg_time = v->status_msg_time;
  E.prefix_state = v->prefix_state;
  E.prompt_input = v->prompt_input;
  E.prompt_is_command = v->prompt_is_command;
  E.quit_times_buffer = v->quit_times_buffer;
  E.which_key_prefix = v->which_key_prefix;
  E.which_key_since = v->which_key_since;
  E.current_project = v->current_project;
  E.constantes = v->constantes;
  projectSwapLastBuffer(&v->project_last_buffer);
  editorSwapScreenCache(&v->cache);
}

// Releases what a saved (not current) view owns.
static void viewFree(ClientView *v) {
  int i;
  gfree(v->layout.panes);
  gfree(v->status_msg);
  for (i = 0; i < v->file_finder.path_count; i++)
    gfree(v->file_finder.paths[i]);
  gfree(v->file_finder.paths);
  gfree(v->file_finder.match);
  gfree(v->file_finder.root);
  gfree(v->cache.frame);
  screenDiffFreeSaved(v->cache.diff);
  memset(v, 0, sizeof(*v));
}

/**
 * @brief Gives a window that was never shown its own single-pane layout,
 * on `buffer_id`. E holds the (empty) fresh view when this runs.
 */
static void viewStart(const DaemonClient *c, const int buffer_id) {
  EditorPane *active;
  E.screenrows = c->rows - 2 > 1 ? c->rows - 2 : 1;
  E.screencols = c->cols > 1 ? c->cols : 1;
  E.layout.panes = NULL;
  splitScreenInit();
  active = splitScreenGetActivePane();
  active->buffer_id = buffer_id;
  E.status_msg = gcalloc(PATH_MAX, sizeof(Utf8Char));
  E.status_msg_time = 0;
  E.prefix_state = 0;
  E.quit_times_buffer = E.constantes.QUIT_TIMES;
  bufferSwitch(buffer_id); // the buffer's own key prefix, if it has one
}

/**
 * @brief Panes showing a buffer another window has killed since this one
 * was last current are moved to one that still exists.
 */
static void viewFixPanes(void) {
  int i;
  for (i = 0; i < E.layout.num_panes; i++)
    if (!bufferFindById(E.layout.panes[i].buffer_id) && E.buffers)
      E.layout.panes[i].buffer_id = E.buffers->buffer_id;
}

/**
 * @brief Makes `c`'s window current (NULL: no window), saving the one that
 * was.
 */
static void clientSwitchTo(DaemonClient *c) {
  int start_buffer;
  if (c == s_current)
    return;
  // A new window starts on whatever the previous view was showing, in its
  // project until projectSyncActive() looks at its own buffer.
  start_buffer = splitScreenGetActivePane() ? splitScreenGetActivePane()->buffer_id : -1;
  if (c && c->view.fresh) {
    c->view.current_project = E.current_project;
    c->view.constantes = E.constantes;
    c->view.project_last_buffer = -1;
  }
  viewSave(s_current ? &s_current->view : &s_idle);
  viewLoad(c ? &c->view : &s_idle);
  s_current = c;
  // Relative paths, the open-file prompt, new terminals: from where this
  // window's `beluga -c` ran.
  E.cwd = c ? c->cwd : NULL;
  if (!c)
    return;
  if (c->view.fresh) {
    c->view.fresh = 0;
    c->size_changed = 0;
    if (!bufferFindById(start_buffer) && E.buffers)
      start_buffer = E.buffers->buffer_id;
    viewStart(c, start_buffer);
  }
  if (c->size_changed) {
    c->size_changed = 0;
    editorApplyScreenSize(c->rows, c->cols);
  }
  viewFixPanes();
}

/***********************************************************************************************************************
 *
 *                                                 Clients
 *
 **********************************************************************************************************************/

static void keysAppend(DaemonClient *c, const unsigned char *bytes, const size_t n) {
  unsigned char *grown;
  size_t cap;
  if (c->keys_head > 0) {
    memmove(c->keys, c->keys + c->keys_head, c->keys_len - c->keys_head);
    c->keys_len -= c->keys_head;
    c->keys_head = 0;
  }
  if (c->keys_len + n > c->keys_cap) {
    cap = c->keys_cap ? c->keys_cap : 256;
    while (cap < c->keys_len + n)
      cap *= 2;
    grown = grealloc(c->keys, cap);
    if (!grown)
      return; // dropped keys, rather than a dead daemon
    c->keys = grown;
    c->keys_cap = cap;
  }
  memcpy(c->keys + c->keys_len, bytes, n);
  c->keys_len += n;
}

static int keysPending(const DaemonClient *c) { return c->keys_head < c->keys_len; }

static int parseSize(const char *text, int *rows, int *cols) {
  if (sscanf(text, "%d %d", rows, cols) != 2)
    return -1;
  if (*rows < 3 || *rows > 2000 || *cols < 1 || *cols > 2000)
    return -1;
  return 0;
}

/**
 * @brief Reads the hello: protocol line, size, working directory, files.
 */
static int handleHello(DaemonClient *c, const unsigned char *payload, const size_t len) {
  char *text = galloc(len + 1);
  char *line;
  char *next;
  int n = 0;
  int i;

  if (!text)
    return -1;
  memcpy(text, payload, len);
  text[len] = '\0';
  for (line = text; line; line = next, n++) {
    next = strchr(line, '\n');
    if (next)
      *next++ = '\0';
    if (n == 0 && strcmp(line, DAEMON_PROTOCOL) != 0)
      break;
    if (n == 1 && parseSize(line, &c->rows, &c->cols) == -1)
      break;
    if (n == 2)
      snprintf(c->cwd, sizeof(c->cwd), "%s", line);
    if (n >= 3 && line[0]) {
      c->files = grealloc(c->files, sizeof(char *) * (size_t)(c->n_files + 1));
      c->files[c->n_files] = galloc(strlen(line) + 1);
      strcpy(c->files[c->n_files++], line);
    }
  }
  gfree(text);
  if (n < 3) {
    for (i = 0; i < c->n_files; i++)
      gfree(c->files[i]);
    c->n_files = 0;
    return -1;
  }
  c->hello_pending = 1;
  return 0;
}

/**
 * @brief beluga --kill: the daemon stops after this tick, unless a file
 * buffer has unsaved changes and the kill isn't forced.
 */
// How many file buffers have unsaved changes.
static int unsavedBuffers(void) {
  struct buffer_t *b = E.buffers;
  int unsaved = 0;
  int i;
  for (i = 0; b && i < E.number_of_buffer; i++, b = b->next)
    if (b->type == FILE_BUFF && b->dirty)
      unsaved++;
  return unsaved;
}

static void handleKill(DaemonClient *c, const int force) {
  char message[160];
  const int unsaved = unsavedBuffers();

  if (unsaved > 0 && !force) {
    snprintf(message, sizeof(message),
             "beluga: %d buffer%s unsaved changes; save them, or use beluga --kill --force",
             unsaved, unsaved == 1 ? " has" : "s have");
    frameSend(c->conn, FRAME_EXIT, message, strlen(message));
  } else {
    frameSend(c->conn, FRAME_EXIT, "", 0);
    s_stop = 1;
  }
  c->gone = 1;
}

/**
 * @brief Reads what `c` has sent, waiting at most `timeout_ms`, and acts
 * on the frames: keys are queued, a resize is noted (and applied at once
 * if `c` is current), a hello opens the window on the next tick.
 */
static void clientPump(DaemonClient *c, const int timeout_ms) {
  char type;
  const unsigned char *payload;
  size_t len;
  char size_text[32];
  int r;

  if (c->gone)
    return;
  if (frameRead(&c->reader, c->conn, timeout_ms) < 0)
    c->gone = 1;
  while ((r = frameNext(&c->reader, &type, &payload, &len)) == 1) {
    if (type == FRAME_HELLO && !c->greeted && !c->hello_pending) {
      if (handleHello(c, payload, len) == -1) {
        frameSend(c->conn, FRAME_EXIT, "beluga: this daemon speaks another protocol version",
                  strlen("beluga: this daemon speaks another protocol version"));
        c->gone = 1;
      }
    } else if (type == FRAME_INPUT && (c->greeted || c->hello_pending)) {
      keysAppend(c, payload, len);
    } else if (type == FRAME_KILL) {
      handleKill(c, len == 5 && memcmp(payload, "force", 5) == 0);
    } else if (type == FRAME_RESIZE && len < sizeof(size_text)) {
      memcpy(size_text, payload, len);
      size_text[len] = '\0';
      if (parseSize(size_text, &c->rows, &c->cols) == 0) {
        if (c == s_current)
          editorApplyScreenSize(c->rows, c->cols);
        else
          c->size_changed = 1;
      }
    }
  }
  if (r < 0)
    c->gone = 1;
  if (c->gone)
    c->closing = 1;
}

/**
 * @brief Opens a window that said hello: its own layout, and the files it
 * asked for (relative to its working directory).
 */
static void clientOpen(DaemonClient *c) {
  char path[OS_PATH_MAX];
  Utf8Char *upath;
  int first = -1;
  int len;
  int id;
  int i;

  c->hello_pending = 0;
  c->greeted = 1;
  c->view.fresh = 1;
  clientSwitchTo(c);
  for (i = 0; i < c->n_files; i++) {
    if (os_path_is_absolute(c->files[i]) || !c->cwd[0])
      len = snprintf(path, sizeof(path), "%s", c->files[i]);
    else
      len = snprintf(path, sizeof(path), "%s/%s", c->cwd, c->files[i]);
    if (len < 0 || (size_t)len >= sizeof(path))
      continue;
    upath = stringToUtf8((unsigned char *)path);
    id = bufferCreate(upath, READ_AND_WRITE, FILE_BUFF);
    gfree(upath);
    if (id >= 0 && first < 0)
      first = id;
  }
  if (first >= 0)
    bufferSwitch(first);
  editorSetStatusMessage("beluga daemon: CTRL-x CTRL-c closes this window, the buffers stay open");
}

static void clientFree(DaemonClient *c) {
  DaemonClient **link;
  int last_buffer = -1;
  int i;
  // The next window to open starts where this one was.
  if (c->greeted) {
    clientSwitchTo(c);
    last_buffer = splitScreenGetActivePane()->buffer_id;
  }
  clientSwitchTo(NULL);
  if (bufferFindById(last_buffer))
    splitScreenGetActivePane()->buffer_id = last_buffer;
  if (c == s_focused)
    s_focused = NULL;
  for (link = &s_clients; *link; link = &(*link)->next)
    if (*link == c) {
      *link = c->next;
      break;
    }
  viewFree(&c->view);
  os_conn_close(c->conn);
  frameReaderFree(&c->reader);
  for (i = 0; i < c->n_files; i++)
    gfree(c->files[i]);
  gfree(c->files);
  gfree(c->keys);
  gfree(c);
}

// Ends a window: tells the client (if it can still hear) and forgets it.
static void clientClose(DaemonClient *c, const char *message) {
  if (!c->gone)
    frameSend(c->conn, FRAME_EXIT, message, strlen(message));
  clientFree(c);
}

static void acceptClients(void) {
  OsConn *conn;
  DaemonClient *c;
  int count = 0;

  for (c = s_clients; c; c = c->next)
    count++;
  while ((conn = os_server_accept(s_server)) != NULL) {
    if (count >= DAEMON_MAX_CLIENTS) {
      frameSend(conn, FRAME_EXIT, "beluga: the daemon has too many windows open",
                strlen("beluga: the daemon has too many windows open"));
      os_conn_close(conn);
      continue;
    }
    c = gcalloc(1, sizeof(*c));
    if (c)
      c->id = s_next_id++;
    if (!c) {
      os_conn_close(conn);
      continue;
    }
    c->conn = conn;
    c->next = s_clients;
    s_clients = c;
    count++;
  }
}

/***********************************************************************************************************************
 *
 *                                                 The screen driver
 *
 **********************************************************************************************************************/

static void daemonWrite(const void *buf, const size_t n) {
  if (!s_current || s_current->gone)
    return;
  if (frameSend(s_current->conn, FRAME_OUTPUT, buf, n) == -1) {
    s_current->gone = 1;
    s_current->closing = 1;
  }
}

/**
 * @brief Key bytes for the current window, waiting up to `timeout_ms` for
 * the client to send some. A window that has gone (or no window at all)
 * reads ESC, so a prompt open on it ends instead of waiting forever.
 */
static long daemonReadKeys(void *buf, const size_t n, const int timeout_ms) {
  DaemonClient *c = s_current;
  const long long deadline = os_monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  long long left;
  size_t avail;

  if (n == 0)
    return 0;
  while (c && !keysPending(c) && !c->gone) {
    left = deadline - os_monotonic_ms();
    if (timeout_ms >= 0 && left <= 0)
      break;
    clientPump(c, timeout_ms < 0 ? 1000 : (int)left);
  }
  if (c && keysPending(c)) {
    avail = c->keys_len - c->keys_head;
    if (avail > n)
      avail = n;
    memcpy(buf, c->keys + c->keys_head, avail);
    c->keys_head += avail;
    return (long)avail;
  }
  if (!c || c->gone) {
    ((unsigned char *)buf)[0] = ESCAPE;
    return 1;
  }
  return 0;
}

static int daemonScreenSize(int *rows, int *cols) {
  if (!s_current)
    return -1;
  *rows = s_current->rows;
  *cols = s_current->cols;
  return 0;
}

static const ScreenDriver s_daemon_driver = {daemonWrite, daemonReadKeys, daemonScreenSize};

/***********************************************************************************************************************
 *
 *                                                 The run loop
 *
 **********************************************************************************************************************/

/**
 * @brief Acts on a Ctrl-C, a closed terminal or a SIGTERM (os_take_stop_signal()).
 * @details Nothing unsaved: the daemon stops. A closed terminal or SIGTERM
 * can't be asked anything, so the unsaved buffers get their autosave copy
 * (offered back when the file is opened again) and the daemon stops. Ctrl-C
 * with unsaved buffers only warns; a second one within a few seconds stops
 * it the same way. Stopping goes through the end of the run loop, which
 * closes the windows and removes the socket.
 */
static void handleStopSignal(void) {
  static long long s_interrupted_at = -1;
  const int sig = os_take_stop_signal();
  const long long now = os_monotonic_ms();
  struct buffer_t *b;
  int unsaved;
  int i;

  if (sig == OS_STOP_NONE)
    return;
  unsaved = unsavedBuffers();
  if (sig == OS_STOP_INTERRUPT && unsaved > 0 &&
      (s_interrupted_at < 0 || now - s_interrupted_at > DAEMON_INTERRUPT_AGAIN_MS)) {
    s_interrupted_at = now;
    fprintf(stderr,
            "\nbeluga: %d buffer%s unsaved changes. Save them, or press Ctrl-C again to "
            "stop anyway (autosave keeps a copy).\n",
            unsaved, unsaved == 1 ? " has" : "s have");
    return;
  }
  b = E.buffers;
  for (i = 0; b && i < E.number_of_buffer; i++, b = b->next)
    if (b->type == FILE_BUFF && b->dirty)
      autosaveWrite(b);
  if (unsaved > 0)
    fprintf(stderr, "beluga: stopped; the unsaved changes of %d buffer%s are in ~/.beluga/autosave\n",
            unsaved, unsaved == 1 ? "" : "s");
  s_stop = 1;
}

static void daemonRunLoop(void) {
  OsStream *streams[RUN_LOOP_MAX_STREAMS];
  int ready[RUN_LOOP_MAX_STREAMS];
  DaemonClient *by_stream[RUN_LOOP_MAX_STREAMS];
  RunLoopWatch watch;
  DaemonClient *c;
  DaemonClient *next;
  int n_streams;
  int i;

  while (!s_stop) {
    // Index 0: the listener; then one per client; then what the
    // standalone loop waits on too.
    n_streams = 0;
    streams[n_streams++] = os_server_stream(s_server);
    for (c = s_clients; c; c = c->next)
      if (!c->gone) {
        by_stream[n_streams] = c;
        streams[n_streams++] = os_conn_input(c->conn);
      }
    i = n_streams;
    editorWatchStreams(&watch, streams, &n_streams, RUN_LOOP_MAX_STREAMS);

    // Replies and process callbacks land in the window that last typed:
    // its completion popup, its status line.
    clientSwitchTo(s_focused);

    if (os_wait(streams, n_streams, 50, ready) > 0) { // 50ms tick
      editorHandleStreams(&watch, ready);
      if (ready[0])
        acceptClients();
      while (--i >= 1)
        if (ready[i])
          clientPump(by_stream[i], 0);
    }
    editorTick();
    handleStopSignal();
    if (s_stop)
      break;

    for (c = s_clients; c; c = next) {
      next = c->next;
      if (c->hello_pending && !c->closing)
        clientOpen(c);
      while (c->greeted && keysPending(c) && !c->closing) {
        clientSwitchTo(c);
        s_focused = c;
        editorProcessKeypress();
        if (E.exit_requested) { // CTRL-x CTRL-c: this window only
          E.exit_requested = 0;
          c->closing = 1;
        }
      }
    }

    for (c = s_clients; c; c = next) {
      next = c->next;
      if (c->closing)
        clientClose(c, "");
      else if (c->greeted) {
        clientSwitchTo(c);
        // The active pane may have moved to another project's buffer.
        projectSyncActive();
        editorRefreshScreen();
      }
    }
  }

  while (s_clients)
    clientClose(s_clients, "beluga: the daemon was stopped");
  editorCloseTerminals();
  os_server_close(s_server);
  s_server = NULL;
  screenSetDriver(NULL);
  s_running = 0;
}

int daemonIsRunning(void) { return s_running; }

/*
 * Debug sessions belong to the window that started them: their events
 * (stopped, output, a program's terminal pane) land in that window's view,
 * whichever window is current when they arrive.
 */
static int windowCurrent(void) { return s_current ? s_current->id : 0; }

static int windowEnter(const int id) {
  DaemonClient *c;
  for (c = s_clients; c; c = c->next)
    if (c->id == id && c->greeted && !c->closing)
      break;
  if (!c || c == s_current)
    return 0;
  s_entered_from = s_current;
  clientSwitchTo(c);
  return 1;
}

static void windowLeave(void) {
  clientSwitchTo(s_entered_from);
  s_entered_from = NULL;
}

int daemonMain(const char *name) {
  char err[512];
  Utf8Char *splash_screen;
  EditorPane *active;

  s_server = os_server_listen(name, err, sizeof(err));
  if (!s_server) {
    fprintf(stderr, "beluga: %s\n", err);
    return 1;
  }
  // Ctrl-C, a closed terminal, SIGTERM: stop the way --kill does, rather
  // than dying with the socket left behind (handleStopSignal()).
  os_catch_stop_signals();
  dapSetWindowHooks(windowCurrent, windowEnter, windowLeave);
  // No window yet: no size (80x24 is assumed), nothing drawn, prompts end.
  screenSetDriver(&s_daemon_driver);
  s_running = 1;
  initEditor();

  splash_screen = gcalloc(PATH_MAX, sizeof(Utf8Char));
  utf8Printf(splash_screen, PATH_MAX, "%s/assets/beluga.txt", utilBelugaDir());
  active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(splash_screen, READ_ONLY, FILE_BUFF);
  gfree(splash_screen);
  // As in main(): the config has loaded and the startup buffer is open.
  runHooks("after-init", lisp_null());
  settingsReportProblems();
  configShowErrors();
  if (configErrorCount() > 0)
    fprintf(stderr, "beluga: %d error(s) in the config, listed in *config-errors*\n",
            configErrorCount());

  if (strcmp(name, DAEMON_DEFAULT_NAME) == 0)
    printf("beluga daemon running. Open a window with: beluga -c [file]\n");
  else
    printf("beluga daemon \"%s\" running. Open a window with: beluga -c --name=%s [file]\n",
           name, name);
  fflush(stdout);

  daemonRunLoop();
  return 0;
}
