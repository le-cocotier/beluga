#ifndef DATA_H_
#define DATA_H_

#include <signal.h> // sig_atomic_t (standard C)
#include <stdio.h>
#include <time.h>

#include "define.h"
#include "lisp.h"
#include "os.h"
#include "jsonrpc_stream.h"

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
    // 0 = default; 1-256 = palette entry n-1 (so 1-8 are ANSI 30-37, 9-16
    // bright 90-97, 17-256 the rest of the 256 colours, 38;5;n);
    // TERM_COLOR_RGB | 0xRRGGBB = truecolour (38;2;r;g;b).
    unsigned int fg;
    unsigned int bg;         // same encoding as fg, for 40-47 / 100-107 / 48;...
    unsigned char bold : 1;
    unsigned char underline : 1;
    unsigned char reverse : 1;
} TermCellAttr;

#define TERM_COLOR_RGB 0x1000000u

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
    // Each pane has its own cursor: a buffer's x/y is the cursor of the
    // pane that has focus, and the others keep theirs here, for the buffer
    // saved_buffer_id (-1: none) — see splitScreenSyncCursor(). `id` tells
    // panes apart across layouts (0 until it is first given one).
    int id;
    int saved_x, saved_y;
    int saved_buffer_id;
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
    char* COLOR_ADDED;   // diff "+" lines
    char* COLOR_REMOVED; // diff "-" lines
    char* DEBUG_LINE_BACKGROUND; // the line the debugger stopped on
} Theme;

// Theme is nothing but these char* fields, freed in a loop (deInitEditor).
#define THEME_COLOR_COUNT ((int)(sizeof(Theme) / sizeof(char *)))

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
    int AUTOSAVE_INTERVAL; // (define AUTOSAVE-INTERVAL 15) — seconds; 0 = off (autosave.c)
    int STICKY_SCROLL;      // (define STICKY-SCROLL #t) — enclosing definitions pinned atop a pane
    int BREADCRUMBS;        // (define BREADCRUMBS #t) — "struct › function" in the status bar
    int INLINE_DIAGNOSTICS; // (define INLINE-DIAGNOSTICS #t) — the message after the line
    int INLAY_HINTS;        // (define INLAY-HINTS #t) — parameter names / types inside the line
    int DEBUG_INLINE_VALUES; // (define DEBUG-INLINE-VALUES #t) — variables' values after the lines, while paused
    int MOUSE;              // (define MOUSE #t) — wheel scrolling and click-to-place
    int EXTENDED_KEYS;      // (define EXTENDED-KEYS #t) — ask for modifyOtherKeys (Ctrl+1, Ctrl+.)
};

/**
 * @brief One project beluga knows about, found by its markers or read
 * back from ~/.beluga/projects. See project.h.
 */
typedef struct
{
    char* root;          // absolute, '/' separators, no trailing slash
    int trusted;         // its .beluga/init.lisp may run (project-trust)
    int opened;          // made current at least once this session
    int removed;         // forgotten (project-remove): not listed or saved
    int has_config_env;  // config_env holds its evaluated .beluga/init.lisp
    Lisp config_env;     // the global env extended with the project's own table
    long last_used;      // larger is more recent; orders ~/.beluga/projects
    struct const_t constantes; // the settings while it's the current project
} Project;

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
    // NULL: global. Otherwise " ext1 ext2 " (space-delimited both ends):
    // the binding only applies in buffers with one of these extensions, and
    // there it wins over a global binding of the same key.
    char* extensions;
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

    // Whole-line rules, checked in order before anything else on a row:
    // a row starting with `prefix` is drawn entirely in `role`'s color.
    // How a diff gets its +/- lines colored (syntax-add-line-prefix).
    struct {
        char prefix[16];
        int role; // a TokenType (syntax_highlighter.h)
    } line_prefixes[12];
    int num_line_prefixes;

    // Inline spans, tried in order where a token can start: `open`, then
    // text on the same row up to `close`, drawn in `role` (the markers in
    // the comment color). Markdown-style flanking: no word character just
    // before `open` or after `close`, no blank just inside either. How
    // Markdown gets **bold** and *italic* (syntax-add-span).
    struct syntax_span_t {
        char open[8];
        char close[8];
        int role; // a TokenType
    } spans[8];
    int num_spans;
} LanguageSyntax;

struct lsp_client_t
{
    // ── Process ───────────────────────────────────────────────────────────────
    // The server, talking JSON-RPC over its stdin/stdout. NULL when none
    // is running.
    OsProc *proc;
    JsonRpcStream recv; // what it sent that isn't a whole message yet
    int completion_just_arrived;
    int completion_requested;
    char language_id[32]; // set from the matching lspServerConfig_t at start
    // The server's argv[0] (e.g. "clangd", "digestif"): what identifies
    // this client — a file belongs to the client whose command its own
    // lsp-server entry names (lspClientForBuffer() in buffer.c).
    char command[OS_PATH_MAX]; // path-sized: servers set by absolute path

    // ── State ─────────────────────────────────────────────────────────────────
    LspState state;
    int next_id;
    // What the server answered `initialize` with when it refused to start
    // (e.g. typescript-language-server without a TypeScript it accepts),
    // "" otherwise. Reported again, and the server retried, at the next
    // file of its language (bufferMaybeStartLsp()).
    char start_error[256];

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
    int inlay_hints;                 // 1 = the server answers textDocument/inlayHint
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

    // ── Document sync ─────────────────────────────────────────────────────────
    // From the initialize response: whether the server takes incremental
    // didChange (textDocumentSync.change == 2), and whether it agreed to
    // UTF-8 positions (positionEncoding "utf-8") or uses the default,
    // UTF-16 code units. See lspDidChange().
    int sync_incremental;
    int utf8_positions;
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
 * @note items/all_items/on_select/on_cancel are raw Lisp values, kept
 * alive and updated by the collector like E.hooks[]/E.key_binds[]: see
 * lisp_gc.c, which also clears them while the popup isn't visible.
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
    char *root;         // the directory paths[] are relative to, owned
} FileFinder;

// Numbered to match LSP's own DiagnosticSeverity (1=Error..4=Hint) exactly,
// so a server's raw severity int can be stored directly with no translation
// step — a mismatch here previously made severity-4 (Hint) diagnostics
// render as invisible blanks in the gutter (fell through to the "no
// diagnostic" case since nothing in the enum matched the value 4).
// One symbol of a document (textDocument/documentSymbol) or of the whole
// project (workspace/symbol).
typedef struct {
  char *name;   // galloc'd
  char *detail; // galloc'd or NULL: the signature (document) or the
                // enclosing scope (workspace)
  char *path;   // galloc'd absolute path, workspace symbols only, else NULL
  int kind;     // LSP SymbolKind, see lspSymbolKindName()
  int depth;    // nesting: 0 at top level, 1 for a struct's fields, ...
  int line;     // 0-based, where the name is
  int col;
  int start_line; // 0-based, the whole symbol (doc comment to closing brace)
  int end_line;
} LspSymbol;

// One inlay hint (textDocument/inlayHint): text the server wants shown
// between the characters of a row without being part of it — a parameter
// name before an argument ("count:"), a deduced type.
typedef struct {
  int line;    // 0-based
  int col;     // the character it is drawn before
  char *label; // galloc'd, padding spaces included
} LspInlayHint;

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
    TPARSE_ESC_INTER, // ESC + intermediate byte (ESC ( B, ESC # 8, ...): one more byte ends it
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
    int csi_private;                  // CSI's '?', '>', '<' or '=' prefix byte, or 0
    int csi_intermediate;             // CSI had an intermediate byte (CSI ! p, CSI SP q, ...)
    int cursor_x;                     // screen column (wide characters take two)
    int cursor_y;                     // row index in the buffer
    int saved_cursor_x, saved_cursor_y;
    TermCellAttr saved_attr;
    int screen_top_row;
    int pane_height;
    // The pane's width, for wrapping (0: not known, lines don't wrap), and
    // xterm's pending wrap: a character went in the last column, and the
    // cursor stays there until the next one goes to the next line.
    int pane_width;
    int wrap_pending;
    // Scroll region (DECSTBM): its first screen row counted from 0, its
    // last counted from 1, 0 meaning the screen's last row whatever the
    // pane's height (so a zeroed state has no region).
    int scroll_top, scroll_bottom;
    // Alternate screen (CSI ? 1049 h, used by vim, less, htop): the normal
    // screen's rows wait here, and buf->row holds the alternate screen.
    int alt_screen;
    Row* main_rows;
    int main_numrows;
    int main_top_row;
    // Where the last character went: the cursor's column, on row
    // col_cache_y, starts at character col_cache_idx (see putChar()).
    int col_cache_valid;
    int col_cache_y, col_cache_x, col_cache_idx;

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
// one entry each, however long the text; it is a galloc'd UTF-8 string
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
    // The pane (EditorPane.id) whose cursor x/y is, and where it was when
    // that pane last lost focus: x/y moved since means something put the
    // cursor somewhere on purpose (a jump), which the next pane keeps.
    int cursor_pane;
    int cursor_left_x, cursor_left_y;
    // Daemon mode: windows waiting in a prompt with this buffer active,
    // while other windows are served. It can't be killed meanwhile.
    int prompt_holds;
    Row* row;
    int numrows;
    int b_has_changed;
    Utf8Char* filename;
    // Highlight as this extension's language instead of the filename's
    // ("" = by filename). Set with buffer-set-syntax, for generated buffers
    // like a diff view.
    char syntax[16];
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
    // Counts every edit, like `dirty`, but is never reset: saving clears
    // `dirty`, and an edit + save inside the sync debounce used to look
    // "already synced" while the version number sent to the server went
    // back to 1. LSP document versions are derived from it (see
    // lspDocumentVersion() in completion.c), so they only ever go up.
    int edit_seq;
    // Value of `edit_seq` as of the last didChange (or didOpen) sent for
    // this buffer. lspSyncDebounceTick() compares the two to notice ANY
    // edit — typing, deleting, undo/redo, paste, kill-region, LSP-applied
    // edits — not just the word-char/trigger-char keystrokes that arm
    // completion.
    int lsp_synced_seq;
    // The text as the server last saw it (didOpen or didChange), so the
    // next didChange can send only the range that changed. NULL until the
    // buffer is opened on a server that syncs incrementally.
    char *lsp_sent_text;
    size_t lsp_sent_len;

    // ── Undo/redo (per-buffer; zero-initialized by bufferCreate's calloc) ──
    UndoList undo_stack;
    UndoList redo_stack;
    int undo_next_group_id;
    // While set (bufferUndoGroupBegin/End), every recorded edit joins the
    // current group, so a multi-edit command undoes in one step.
    int undo_group_hold;
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

    // ── LSP decorations ──────────────────────────────────────────────────
    // The server's symbols (sticky scroll, breadcrumbs) and inlay hints for
    // this buffer, asked for in the background once the server has caught
    // up with an edit (lspDecorationsTick()). Between an edit and the
    // answer, rows may have moved: they're drawn as they are, a moment
    // stale, rather than blinking off.
    LspSymbol* lsp_symbols;
    int lsp_symbols_count;
    LspInlayHint* lsp_hints; // sorted by (line, col)
    int lsp_hints_count;
    int lsp_decor_seq; // edit_seq + 1 they were asked for at; 0 = never
    // Struct layout notes (lspToggleStructLayout()), one per line, drawn
    // after it while lsp_layout_seq is still edit_seq + 1.
    LspInlayHint* lsp_layout;
    int lsp_layout_count;
    int lsp_layout_seq;

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

    // Autosave (autosave.c): `dirty` as of the last copy written, and when
    // it was written.
    int autosaved_dirty;
    time_t autosave_time;

    // The file as last read or written (file_io.c, fileChangedOnDisk()):
    // disk_known is 0 for a file that didn't exist yet. disk_conflict: it
    // changed on disk and the user kept their version, so saving asks.
    int disk_known;
    long long disk_mtime;
    long long disk_size;
    int disk_conflict;

    // buffer-local-set / buffer-local (builtins_edit.c): an alist of
    // (symbol . value). lisp_null() — all zero — when empty.
    Lisp locals;
    // The buffer's project: index + 1 into E.projects, 0 while not looked
    // up yet, -1 for none. Resolved once, by projectActivateForBuffer().
    int project;

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

    // One client per language server (keyed by its command), created the
    // first time a file for it is opened — see bufferMaybeStartLsp() and
    // lspClientForBuffer() in buffer.c. Several run side by side.
    LspClient* lsp_clients[LSP_MAX_CLIENTS];
    int number_of_lsp_clients;
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

    // The working directory relative paths, the open-file prompt and new
    // terminal panes start from (editorGetCwd()). NULL: the process's. In
    // the daemon, the current window's (from its client).
    const char* cwd;
    // which-key (which_key.c): the prefix it last saw pending, and since
    // when (ms), so the panel shows only once a chord has settled.
    int which_key_prefix;
    long which_key_since;

    // ── Projects (project.c) ────────────────────────────────────────────
    // constantes is the current project's copy of the settings;
    // global_constantes is what init.lisp alone set, for projects
    // without a config of their own.
    struct const_t global_constantes;
    Project* projects;
    int number_of_projects;
    int current_project; // index into projects, -1 for none

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
    // TAB completion's matches in a path prompt, shown in the help panel
    // while TAB is being pressed (borrowed from editorPrompt(), NULL
    // otherwise), and the one the prompt holds (-1: their common prefix).
    char** prompt_matches;
    int prompt_match_count;
    int prompt_match_selected;

    int exit_requested;

    // initialized: init.lisp has finished loading (set by initEditor()).
    // batch: no screen, messages go to stdout (beluga --sync-packages).
    int initialized;
    int batch;
    // Screen cell (0-based, row 0 = top line) of the last mouse report,
    // set by readUtf8Char() alongside the WHEEL-*/MOUSE-* key it returns.
    int mouse_row;
    int mouse_col;

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
