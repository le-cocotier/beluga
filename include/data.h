#ifndef DATA_H_
#define DATA_H_

#include <signal.h>
#include <stdio.h>
#include <sys/types.h>  // pid_t, used by lsp_client_t / buffer_t below
#include <termios.h>
#include <time.h>

#include "define.h"
#include "lisp.h"

typedef struct lsp_client_t LspClient;

/***********************************************************************************************************************
 *
 *                                                   utf8 data
 *
 **********************************************************************************************************************/

/**
 * @struct Utf8Char
 * @brief Contains a single utf8 character.
 */
typedef struct utf8_char_t
{
    unsigned char character[4];
    char length; // must be between 1 and 4
} Utf8Char;



/**
 * @brief Per-character terminal rendering attributes (color/style),
 * only populated for rows belonging to a TERMINAL_BUFF buffer.
 */
typedef struct term_cell_attr_t
{
    unsigned char fg;        // 0 = default, 1-8 = ANSI 30-37, 9-16 = bright 90-97
    unsigned char bg;        // same encoding as fg, for 40-47 / 100-107
    unsigned char bold : 1;
    unsigned char underline : 1;
    unsigned char reverse : 1;
} TermCellAttr;

/**
 * \struct Row
 * \brief Store one editor row
 * */
typedef struct row_t
{
    int size; /**< Size of the line */
    Utf8Char* characters;
    TermCellAttr* attrs;
} Row;

typedef struct context_buffer_t
{
    int editor_x, editor_y;
    int width, height;
    Row* rows;
} ContextBuffer;

/**
 * @brief Split modes for screen layout
 */
typedef enum
{
    SPLIT_NONE = 0, // Single buffer fullscreen
    SPLIT_VERTICAL, // Left-right split
    SPLIT_HORIZONTAL // Top-bottom split
} SplitMode;

/**
 * @brief Represents an editor viewport/pane
 */
typedef struct editor_pane_t
{
    int buffer_id; // Which buffer this pane displays
    int height; // Height of this pane
    int width; // Width of this pane
    int origin_x, origin_y;
    int cursor_x; // Local cursor x in this pane
    int cursor_y; // Local cursor y in this pane
    int x_offset, y_offset;
    int is_active; // Is this pane currently active
} EditorPane;

/**
 * @brief Screen layout manager
 */
typedef struct
{
    SplitMode mode;
    EditorPane* panes;
    int num_panes;
    int active_pane; // Index of active pane
} ScreenLayout;


typedef struct theme
{
    char* BACKGROUND_COLOR;
    char* COLOR_KEYWORD;
    char* COLOR_TYPE;
    char* COLOR_STRING;
    char* COLOR_COMMENT;
    char* COLOR_NUMBER;
    char* COLOR_DEFAULT;
} Theme;

enum bufferStatus_e
{
    IDLE,
    READ_ONLY,
    READ_AND_WRITE,
};

struct const_t
{
    int TAB_LENGTH;
    int QUIT_TIMES;
    int LSP;
    int LINE_NUMBERS;  // (define LINE-NUMBERS #t) — draw the line-number gutter
    int WHICH_KEY;     // (define WHICH-KEY #t)    — pending-prefix / M-x help panel
};

struct prefix_t
{
    char prefix_name[64];
    int prefix_id;
    // (add-prefix "name" #t) — a sticky prefix stays active across
    // keystrokes that don't themselves change it, instead of
    // executeKeyBind() auto-resetting to "no-prefix" after every command.
    // "user" (the CTRL-x <key> style, one-shot by design) and "term"
    // (nothing but ESCAPE is ever mapped under it, so the reset never
    // actually fires) don't need this; a prefix meant for continuous
    // navigation while it's active (e.g. arrow keys in a sidebar) does.
    int sticky;
};

struct keyBind_t
{
    char* key_sequence;
    int prefix_id;
    Lisp command;
};

/**
 * @brief One (event-name, callback) registration made via the Lisp
 * `add-hook` builtin. Several bindings may share the same event_name —
 * runHooks() calls all of them, in registration order.
 */
struct hookBind_t
{
    char event_name[64];
    Lisp callback;
};

typedef enum
{
    LSP_NOT_STARTED = 0,
    LSP_INITIALIZING,
    LSP_READY,
    LSP_SHUTTING_DOWN,
    LSP_SHUTDOWN,
} LspState;

typedef struct
{
    int id;
    void (*callback)(struct lsp_client_t* lsp, const char* json);
} LspPending;

// One (extension -> server) mapping registered via the Lisp `lsp-server`
// builtin, e.g. (lsp-server "py" "python" "pyright-langserver" "--stdio").
#define LSP_SERVER_MAX_ARGS 8
struct lspServerConfig_t
{
    char extension[16];
    char language_id[32];
    char* argv[LSP_SERVER_MAX_ARGS + 1]; // NULL-terminated, for execvp
};

struct lsp_client_t
{
    // ── Process ───────────────────────────────────────────────────────────────
    pid_t pid;
    int write_to_server_fd;
    int read_from_server_fd;
    char* recv_buf;
    size_t recv_len;
    size_t recv_cap;
    int completion_just_arrived;
    int completion_requested;
    char language_id[32]; // set from the matching lspServerConfig_t at start
    // The running server's argv[0] (e.g. "clangd", "digestif"), set
    // alongside language_id at start — see lspServerConfigMatchesRunning()
    // in buffer.c for why this exists: "only one language server runs at
    // a time" means a second file whose *own* registered server isn't
    // this one must not be forwarded to it anyway.
    char command[64];

    // ── State ─────────────────────────────────────────────────────────────────
    LspState state;
    int next_id;

    // ── Pending requests ──────────────────────────────────────────────────────
    LspPending pending[LSP_MAX_PENDING];
    int pending_count;

    // ── Completion context ────────────────────────────────────────────────────
    int completion_cursor_x; // screen position when
    int completion_cursor_y; // completion was requested

    // ── Completion debounce / staleness ───────────────────────────────────────
    int completion_debounce_pending;       // 1 = a request is scheduled but not sent yet
    long completion_debounce_deadline_ms;  // monotonic ms timestamp to fire at
    int completion_latest_request_id;      // id of the most recently *sent* completion
                                            // request; responses for older ids are stale

    // ── Completion trigger characters ─────────────────────────────────────────
    // Populated from the server's own initialize response. Falls back to
    // "." alone if the server didn't advertise any — without this,
    // punctuation-triggered completion (the dominant trigger in most
    // languages: "obj." in Python, "->"/"::" in C++) never fires; only
    // typing a following word-char keystroke did.
    char trigger_chars[LSP_MAX_TRIGGER_CHARS][LSP_TRIGGER_CHAR_MAXLEN];
    int num_trigger_chars;
};

typedef struct
{
    char label[128]; // display text  e.g. "printf"
    char insertText[128];
    char detail[64]; // type/sig hint e.g. "int (const char *, ...)"
    int kind; // LSP CompletionItemKind (1=Text,2=Method,3=Function…)
} CompletionItem;

typedef struct
{
    CompletionItem items[COMPLETION_MAX_ITEMS];
    int count;
    int selected; // currently highlighted row
    int visible; // is the popup shown?
    int origin_x; // screen col where popup appears
    int origin_y; // screen row where popup appears
} CompletionPopup;

/**
 * @brief State for the CTRL-x b buffer switcher popup.
 * @note Deliberately holds no snapshot array of buffers — E.buffers is a
 * circular doubly-linked list with no fixed capacity, so navigation just
 * walks it live via ->next/->prev from current_buffer_id, and the popup
 * is rendered by walking it fresh every frame. No static buffer count.
 */
typedef struct
{
    int visible;
    int current_buffer_id; // the buffer currently highlighted
} BufferSwitcherPopup;

/**
 * @brief State for the generic (editor-list-popup ...) builtin — a
 * single-instance modal picker any Lisp package can drive: pass a title,
 * a list of (label . value) pairs, and an on-select/on-cancel callback.
 * @note items/on_select/on_cancel are raw Lisp values kept alive by being
 * reachable from this global, matching how E.hooks[]/E.key_binds[] already
 * retain Lisp callbacks long-term — safe here because lisp_collect() (the
 * GC's moving compactor) is only ever invoked twice, during startup's
 * lisp_lib_load(), and never again once the editor is running.
 */
typedef struct
{
    int visible;
    char title[128];
    Lisp items;      // proper list of (label . value) pairs
    int count;        // cached length of items
    int selected;      // 0-based index into items
    Lisp on_select;    // (lambda (value) ...), called with the chosen pair's cdr
    Lisp on_cancel;    // (lambda () ...) or lisp_null() — called on ESC
} ListPopup;

// Numbered to match LSP's own DiagnosticSeverity (1=Error..4=Hint) exactly,
// so a server's raw severity int can be stored directly with no translation
// step — a mismatch here previously made severity-4 (Hint) diagnostics
// render as invisible blanks in the gutter (fell through to the "no
// diagnostic" case since nothing in the enum matched the value 4).
typedef enum { DIAG_ERROR = 1, DIAG_WARNING, DIAG_INFO, DIAG_HINT } DiagSeverity;

typedef struct
{
    int buffer_id;
    int line; // 0-based
    int col_start; // 0-based
    int col_end;
    DiagSeverity severity;
    char message[256];
} Diagnostic;

typedef struct
{
    Diagnostic entries[DIAG_MAX];
    int count;
} DiagnosticList;

typedef enum
{
    TPARSE_GROUND,
    TPARSE_ESC,
    TPARSE_CSI,
    TPARSE_OSC,       // consuming/discarding an OSC string
    TPARSE_OSC_ESC,   // saw ESC while inside OSC — might be the ST terminator (ESC \)
} TermParseState;

/**
 * @brief Live parser state for one PTY's output stream — cursor position,
 * current SGR attribute, and CSI-sequence-in-progress buffer.
 */
typedef struct term_parser_state_t
{
    TermParseState state;
    int params[CSI_MAX_PARAMS];
    int nparams;
    int cursor_x, cursor_y;           // relative to buf->numrows origin
    int saved_cursor_x, saved_cursor_y;
    int screen_top_row;
    int pane_height;

    TermCellAttr current_attr;

    // Multi-byte UTF-8 reassembly across possibly-fragmented read() calls
    unsigned char utf8_pending[4];
    int utf8_pending_len;
    int utf8_expected_len;
} TermParserState;

// EXPLORER_BUFF: a read-only buffer whose rows are set programmatically
// from Lisp (editor-scratch-set-lines), not loaded from or saved to a
// real file — used for the file-explorer sidebar and for one-shot
// generated content like a project tree dump.
enum buffer_type { FILE_BUFF, TERMINAL_BUFF, EXPLORER_BUFF };

// ── Undo/redo ────────────────────────────────────────────────────────────
//
// Each entry records one atomic, self-inverting row-level edit. Direction
// (undo vs redo) is decided by which function is replaying the entry, not
// by the entry itself — see bufferUndo()/bufferRedo() in buffer.c:
//   INSERT_CHAR (row,col,ch): undo deletes ch at (row,col); redo re-inserts it.
//   DELETE_CHAR (row,col,ch): undo re-inserts ch at (row,col); redo deletes it.
//   SPLIT_LINE  (row,col):    undo joins row+1 back into row at col;
//                             redo splits row at col again.
//   JOIN_LINE   (row,col):    undo splits row at col (recreating row+1);
//                             redo joins row+1 back into row.
//   INSERT_ROW  (row):        undo removes the (blank) row at `row`;
//                             redo re-inserts a blank row there.
// Consecutive entries sharing a group_id are undone/redone together in one
// user-facing call — see undoRecord()'s contiguity check for how a group
// is extended vs. broken.
typedef enum
{
    UNDO_INSERT_CHAR,
    UNDO_DELETE_CHAR,
    UNDO_SPLIT_LINE,
    UNDO_JOIN_LINE,
    UNDO_INSERT_ROW,
} UndoKind;

typedef struct
{
    UndoKind kind;
    int row, col;
    Utf8Char ch; // meaningful for INSERT_CHAR / DELETE_CHAR only
    int group_id;
} UndoEntry;

typedef struct
{
    UndoEntry* entries;
    int count;
    int capacity;
} UndoList;

// One line's git working-tree status, cached on the buffer — see
// bufferRefreshGitInfo() in buffer.c.
typedef enum { GIT_LINE_NONE = 0, GIT_LINE_ADDED, GIT_LINE_MODIFIED } GitLineStatus;

typedef struct buffer_t
{
    enum buffer_type type;
    int buffer_id;
    int b_lsp_open;
    int x, y; /**< Position in the file */
    Row* row;
    int numrows;
    int b_has_changed;
    Utf8Char* filename;
    Utf8Char* path;
    Utf8Char* fullname;

    pid_t child_pid;              // -1 if not a terminal buffer / process exited
    int pty_master_fd;            // -1 if not a terminal buffer
    TermParserState term_state;

    enum bufferStatus_e state;
    int dirty; /**< Has this buffer been modified since last save */

    // ── Undo/redo (per-buffer; zero-initialized by bufferCreate's calloc) ──
    UndoList undo_stack;
    UndoList redo_stack;
    int undo_next_group_id;
    int undo_group_open;    // can the tail of undo_stack still be extended?
    UndoKind undo_last_kind;
    int undo_last_row, undo_last_col;

    // ── Selection (mark & region) ───────────────────────────────────────
    int mark_active;
    int mark_x, mark_y;

    // ── Git integration (modeline branch + per-line diff gutter) ────────
    // Refreshed on buffer-open and after-save (see bufferRefreshGitInfo()),
    // not every frame — shelling out to git on every ~50ms redraw tick
    // would be far too slow.
    int git_available;         // 1 once a branch name has been resolved
    char git_branch[64];
    int git_dirty;             // `git status --porcelain` non-empty for this file
    GitLineStatus* git_lines;  // one entry per row, NULL until refreshed
    int git_lines_count;       // rows git_lines[] currently covers

    // ── Syntax highlighter: block-comment state cache ────────────────────
    // highlight_line() only ever sees whichever rows are currently on
    // screen — it has no way to know, from a mid-file scroll position,
    // whether row Y starts inside an unclosed "/* ... */" that opened
    // somewhere above the visible window. comment_state[Y] answers that:
    // 1 if row Y starts inside such a comment, computed by scanning the
    // whole buffer from the top. Invalidated (comment_state_valid = 0) on
    // any edit and recomputed lazily, once, the next time it's read —
    // see ensureCommentStateFresh() in syntax_highlighter.c.
    unsigned char* comment_state;
    int comment_state_numrows;  // rows comment_state[] currently covers
    int comment_state_valid;

    // Which key prefix to activate when this buffer gets focus, for
    // buffers that come with their own keymap. Only meaningful for
    // EXPLORER_BUFF, which used to imply "explorer" and nothing else —
    // now the explorer sidebar and the search results buffer are the
    // same kind of read-only scratch buffer wanting two different
    // keymaps, so the name lives on the buffer instead of being
    // hardcoded by type. Empty means "explorer", which is what every
    // scratch buffer got before this existed.
    char focus_prefix[64];

    struct buffer_t* next;
    struct buffer_t* prev;

} Buffer;

/**
 * \struct editorConfig
 * \brief Containing our editor state.
 */
typedef struct editor_config_t
{
    int cursor_x, cursor_y; /**< Cursor position */
    int screenrows; /**< Terminal height*/
    int screencols; /**< Terminal width*/

    ScreenLayout layout;

    LspClient* lsp_client;
    CompletionPopup lsp_completion;
    DiagnosticList lsp_diagnostics;

    BufferSwitcherPopup buffer_switcher;
    ListPopup list_popup;

    struct lspServerConfig_t* lsp_servers;
    int number_of_lsp_servers;

    int dirty;

    Utf8Char* status_msg;
    time_t status_msg_time;
    struct termios orig_termios; /**< Terminal communication interface */

    struct const_t constantes;
    int quit_times_buffer;

    char* init_file_path;
    FILE* fd_init_file;
    LispContext ctx; /** Lisp context */
    Lisp ctx_data; /** Lisp data context */
    LispError ctx_error; /** Lisp ctx error */

    struct keyBind_t* key_binds;
    int number_of_keybinds;

    struct hookBind_t* hooks;
    int number_of_hooks;

    struct prefix_t* prefix;
    int number_of_prefix;
    int prefix_state;

    // ── which-key style help ────────────────────────────────────────────
    // prompt_input borrows editorPrompt()'s in-progress buffer while a
    // prompt is up (NULL otherwise) so the help panel can filter live on
    // what's been typed; prompt_is_command marks the M-x prompt
    // specifically, since that's the only one whose candidates are
    // commands rather than file paths or free text.
    Utf8Char* prompt_input;
    int prompt_is_command;

    int exit_requested;

    Buffer *buffers;
    int number_of_buffer;

    Theme theme;

    volatile sig_atomic_t winch_pending; // set by SIGWINCH handler, processed in main loop
} EditorConfig;

/**
 * \struct abuf
 * \brief Contains text to add before writing to screen.
 * */
struct abuf
{
    char* b; /**< Text that will be printed */
    int len; /**< Length of the text */
};


#endif
