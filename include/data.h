#ifndef DATA_H_
#define DATA_H_

#include <signal.h> // sig_atomic_t (standard C)
#include <stdio.h>
#include <time.h>

#include "define.h"
#include "lisp.h"
#include "os.h"

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
    // The buffer the four fields above were last fitted to. When buffer_id
    // no longer matches (any buffer switch, however it happened), the view
    // is recomputed around the new buffer's own cursor instead of keeping
    // the scroll position of whatever the pane showed before — see
    // paneFitShownBuffer() in output.c. -1 forces a fit.
    int shown_buffer_id;
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
    char* LINE_CURSOR_BACKGROUND;
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
    int LSP_SIGNATURE_HELP; // (define LSP-SIGNATURE-HELP #t) — signature popup on "(" and ","
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
    char* argv[LSP_SERVER_MAX_ARGS + 1]; // NULL-terminated, for os_proc_spawn()
};

// ── Syntax highlighting: per-language rules ──────────────────────────────────
// One keyword or type name registered via the Lisp `syntax-add-keyword` /
// `syntax-add-type` builtins. is_type picks which of the two theme roles
// (COLOR_KEYWORD vs COLOR_TYPE) an identifier matching `word` renders in —
// see get_color() in syntax_highlighter.c.
typedef struct
{
    char word[32];
    int is_type;
} SyntaxWord;

// A language's highlighting rules for one file extension, registered via
// (syntax-define-language "ext" line-comment block-open block-close
// quote-chars) and built up with repeated syntax-add-keyword/
// syntax-add-type calls — see builtins.c. highlight_line() in
// syntax_highlighter.c is entirely generic over this: there is nothing
// C-specific left in the highlighter itself, only in whatever the "c"
// package happens to register.
//
// An empty line_comment/block_comment_open/block_comment_close means "this
// language doesn't have that kind of comment at all" (e.g. YAML has no
// block comment) rather than "not configured yet" — the highlighter and
// the cross-row comment-state cache both treat an empty delimiter as
// something that can never match, so there's nothing to special-case at
// the call sites.
typedef struct
{
    char extension[16];
    SyntaxWord* words;
    int num_words;
    int words_cap;

    char line_comment[8];        // e.g. "//" or "#"
    char block_comment_open[8];  // e.g. "/*"
    char block_comment_close[8]; // e.g. "*/"

    // Characters that open a string/char literal, checked one at a time —
    // e.g. "\"'" for PHP/JS (both quote styles), "\"" for languages with
    // only double-quoted strings.
    char quote_chars[8];
    int num_quote_chars;
} LanguageSyntax;

struct lsp_client_t
{
    // ── Process ───────────────────────────────────────────────────────────────
    // The server, talking JSON-RPC over its stdin/stdout. NULL when none
    // is running.
    OsProc *proc;
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

    // ── Document sync debounce ────────────────────────────────────────────────
    // Separate from the completion debounce above: fires a plain didChange
    // (no completion request) whenever the active buffer's `dirty` counter
    // has moved past what was last sent, regardless of which edit caused
    // it — see lspSyncDebounceTick() in completion.c.
    int sync_debounce_pending;       // 1 = a resync is scheduled but not sent yet
    long sync_debounce_deadline_ms;  // monotonic ms timestamp to fire at
    int sync_debounce_buffer_id;     // which buffer the pending resync is for

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
    char insertText[128]; // textEdit.newText when the server sent one
    char detail[64]; // type/sig hint e.g. "int (const char *, ...)"
    int kind; // LSP CompletionItemKind (1=Text,2=Method,3=Function…)
    // The range the server wants insertText to replace (textEdit.range, or
    // InsertReplaceEdit.replace) — the already-typed prefix, usually.
    // has_edit = 0 when the item carried no textEdit: accepting it then
    // replaces the word before the cursor instead.
    int has_edit;
    int edit_line;
    int edit_start_col;
    int edit_end_col;
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
    Lisp items;      // the list currently shown/navigated — all of all_items
                       // when not filterable, a live subset of it when filterable
    int count;        // cached length of items
    int selected;      // 0-based index into items
    Lisp on_select;    // (lambda (value) ...), called with the chosen pair's cdr
    Lisp on_cancel;    // (lambda () ...) or lisp_null() — called on ESC

    // ── optional live filtering (5th arg to editor-list-popup) ──────────
    // all_items is the complete, never-mutated list passed in; items above
    // is rebuilt from it (case-insensitive substring match on each pair's
    // label) every time query changes. Rebuilding a Lisp list per
    // keystroke is fine at the scale this popup is meant for (a file's
    // outline, a handful of environments, buffer names, ...) — a picker
    // over thousands of items (find-file's project-wide file list) wants
    // its own C-side snapshot instead, same reasoning file_finder.c
    // already documents for why it isn't built on this popup.
    int filterable;
    Lisp all_items;
    char query[128];
    int query_len;
} ListPopup;

/**
 * @brief State for the (find-file) fuzzy project-file picker — a modal
 * popup that snapshots every file in the project once on open, then
 * narrows the list live as you type and opens the highlighted one on
 * ENTER. Its own module (file_finder.c) rather than a mode on the generic
 * list picker: the snapshot is a flat char* array so filtering every
 * keystroke is a plain C scan, not a walk of a Lisp list.
 */
typedef struct
{
    int visible;
    char **paths;      // every project file, root-relative, owned
    int path_count;
    int *match;         // indices into paths[] passing the current query, best first
    int match_count;
    char query[256];    // what's been typed so far
    int query_len;      // bytes in query (never splits a UTF-8 char)
    int selected;       // index into match[] (0-based)
} FileFinder;

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

/**
 * @brief Floating text box for LSP hover and signature help.
 * @details Lines are stored already cleaned up (markdown fences and
 * backticks stripped). hl_* marks a byte range of one line drawn in the
 * accent color — the active parameter of a signature.
 */
typedef enum { LSP_INFO_HOVER, LSP_INFO_SIGNATURE } LspInfoKind;

typedef struct
{
    char lines[LSP_INFO_MAX_LINES][LSP_INFO_LINE_W];
    int count;
    int visible;
    LspInfoKind kind;
    int anchor_x; // screen col of the cursor when it was requested
    int anchor_y; // screen row of the cursor when it was requested
    int hl_line;  // -1 when nothing is highlighted
    int hl_start;
    int hl_end;
} LspInfoPopup;

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
//   INSERT_TEXT (row,col,text): undo deletes `text` starting at (row,col);
//                             redo inserts it again.
//   DELETE_TEXT (row,col,text): undo inserts `text` at (row,col); redo
//                             deletes it again.
// The two TEXT kinds cover bulk edits (paste, region delete, LSP edits) in
// one entry each, however long the text; it is a gmalloc'd UTF-8 string
// ('\n' between lines) owned by the entry.
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
    UNDO_INSERT_TEXT,
    UNDO_DELETE_TEXT,
} UndoKind;

typedef struct
{
    UndoKind kind;
    int row, col;
    Utf8Char ch; // meaningful for INSERT_CHAR / DELETE_CHAR only
    char* text;  // meaningful for INSERT_TEXT / DELETE_TEXT only
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

    OsPty *pty;                   // the shell, for a terminal buffer; NULL otherwise
    TermParserState term_state;

    enum bufferStatus_e state;
    int dirty; /**< Has this buffer been modified since last save */
    // The file was loaded with CRLF line endings: the rows hold the lines
    // without their '\r', and editorSave() writes "\r\n" back. New files
    // and LF files are written with "\n" (on Windows too).
    int crlf;
    // Value of `dirty` as of the last didChange (or didOpen) sent for this
    // buffer. lspSyncDebounceTick() compares the two to notice ANY edit —
    // typing, deleting, undo/redo, paste, kill-region, LSP-applied edits —
    // not just the word-char/trigger-char keystrokes that arm completion.
    int lsp_synced_dirty;

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
    // 1 if row Y starts inside such a comment. Only a prefix is ever kept
    // valid: an edit to row N truncates it to rows [0, N], and a read of
    // row Y extends it just far enough to cover Y — so typing in a big
    // file never rescans more than the rows between the edit and the top
    // of the screen. See ensureCommentState() in syntax_highlighter.c.
    unsigned char* comment_state;
    int comment_state_cap;       // slots allocated in comment_state[]
    int comment_state_known;     // rows [0, known) hold a valid entry
    int comment_state_rows_seen; // numrows as of the last tracked change

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
 * \struct EditorConfig
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
    LspInfoPopup lsp_info;

    BufferSwitcherPopup buffer_switcher;
    ListPopup list_popup;
    FileFinder file_finder;

    struct lspServerConfig_t* lsp_servers;
    int number_of_lsp_servers;

    // One entry per extension registered via (syntax-define-language ...).
    LanguageSyntax* languages;
    int number_of_languages;

    int dirty;

    Utf8Char* status_msg;
    time_t status_msg_time;

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
    int bf_size; /**< Total length of buffer */
};


#endif
