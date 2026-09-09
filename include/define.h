#ifndef DEFINE_H_
#define DEFINE_H_

#define CTRL_KEY(k) ((k) & 0x1f)
#define ALT(k) ((k) | 0x200)  // set bit 9 to mark as Option combo


#define ESCAPE '\x1b'
#define CURSOR_TOP_LEFT "\x1b[H"
#define HIDE_CURSOR "\x1b[?25l"
#define SHOW_CURSOR "\x1b[?25h"
#define ERASE_END_LINE "\x1b[K"
#define TAB "\t"
#define SPACE "\x20"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define LSP_MAX_PENDING 64

// Completion trigger strings advertised by the server itself (initialize
// response's completionProvider.triggerCharacters — e.g. "." for Python,
// ".", "->", "::" for clangd). Each is short (1-3 bytes) so a handful of
// small fixed buffers is simpler than reaching for cJSON-backed storage.
#define LSP_MAX_TRIGGER_CHARS 8
#define LSP_TRIGGER_CHAR_MAXLEN 4

#define COMPLETION_MAX_ITEMS  16
#define COMPLETION_MAX_WIDTH  40
#define COMPLETION_POPUP_H    8   // max visible rows
#define DIAG_MAX 128

// Generic (editor-list-popup ...) picker — max visible rows before it
// scrolls, and the widest a label is allowed to render before truncation.
#define LIST_POPUP_MAX_H      16
#define LIST_POPUP_MAX_WIDTH  72

// Idle time after the last word-char keystroke before a completion request
// is actually sent to the server. Resets on every trigger keystroke, so a
// fast typist only sends one didChange+completion round-trip per pause,
// not one per character.
#define LSP_COMPLETION_DEBOUNCE_MS 120

#define GUTTER_WIDTH 2

// Minimum digits in the line-number gutter. Keeps the text column steady
// for short files instead of shifting as the count crosses 10 / 100.
#define LINE_NUMBER_MIN_WIDTH 3


#define ARROW_LEFT   0x80
#define ARROW_RIGHT  0x81
#define ARROW_UP     0x82
#define ARROW_DOWN   0x83
#define PAGE_UP      0x84
#define PAGE_DOWN    0x85
#define DEL_KEY      0x86
#define BEG_LINE     0x87   // HOME
#define END_LINE     0x88   // END
#define ALT_X        0x89   // Alt/Option-x — "execute command" (M-x style)

#define BACKSPACE    127    // 0x7F (DEL byte many terminals send for Backspace)

#define ABUF_INIT {NULL, 0, 0}

#define BELUGA_VERSION "3.1"

#ifdef __APPLE__
#define CLIPBOARD_COPY_CMD  "pbcopy"
#define CLIPBOARD_PASTE_CMD "pbpaste"
#else
#define CLIPBOARD_COPY_CMD  "xclip -selection clipboard"
#define CLIPBOARD_PASTE_CMD "xclip -selection clipboard -o"
#endif


#define CSI_MAX_PARAMS 16

#endif // DEFINE_H_
