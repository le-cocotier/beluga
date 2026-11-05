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
// Language servers running at once (one per server command).
#define LSP_MAX_CLIENTS 8

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
#define LSP_INFO_MAX_LINES 12
#define LSP_INFO_LINE_W 160

// Generic (editor-list-popup ...) picker — max visible rows before it
// scrolls, and the widest a label is allowed to render before truncation.
#define LIST_POPUP_MAX_H      16
#define LIST_POPUP_MAX_WIDTH  72

// Idle time after the last word-char keystroke before a completion request
// is actually sent to the server. Resets on every trigger keystroke, so a
// fast typist only sends one didChange+completion round-trip per pause,
// not one per character.
#define LSP_COMPLETION_DEBOUNCE_MS 120

// Idle time after ANY edit (not just the word-char/trigger-char keystrokes
// that arm completion) before the buffer's full text is resynced to the
// server via didChange. This is what keeps diagnostics current after e.g.
// deleting a line with backspace/DEL/kill-region/undo — none of those go
// through lspScheduleCompletion, so without a separate debounce here the
// server's document (and therefore its diagnostics) would only ever catch
// up on the next completion-triggering keystroke, if any.
#define LSP_SYNC_DEBOUNCE_MS 250

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
#define UNKNOWN_KEY      0x8F   // a recognized-but-unbound sequence (F-keys, Insert, ...):
                                // not printable, so it does nothing instead of
                                // being typed or acting as ESC
// Function keys: F1-F12 are FUNCTION_KEY + 0..11 ("F1".."F12" to map-key),
// Shift+F1-F12 SHIFT_FUNCTION_KEY + 0..11 ("SHIFT-F1".."SHIFT-F12"). Like
// the keys above, they sit in 0x80-0xBF, where no UTF-8 character starts.
#define FUNCTION_KEY        0x90
#define SHIFT_FUNCTION_KEY  0xA0
#define IS_FUNCTION_KEY(c)  (((c) >= FUNCTION_KEY && (c) < FUNCTION_KEY + 12) || \
                             ((c) >= SHIFT_FUNCTION_KEY && (c) < SHIFT_FUNCTION_KEY + 12))
// Mouse reports (see readUtf8Char()); where the pointer was is left in
// E.mouse_row / E.mouse_col.
#define WHEEL_UP         0x8D   // "WHEEL-UP" to map-key
#define WHEEL_DOWN       0x8E   // "WHEEL-DOWN" to map-key
#define MOUSE_LEFT       0x9C   // left button pressed — "MOUSE-LEFT" to map-key
#define MOUSE_OTHER      0x9D   // any other mouse report (release, middle/right button): ignored
#define IS_MOUSE_KEY(c)  ((c) == WHEEL_UP || (c) == WHEEL_DOWN || (c) == MOUSE_LEFT || \
                          (c) == MOUSE_OTHER)
#define MOUSE_SCROLL_LINES 3     // lines per wheel notch
// Mouse reporting on/off, sent with every frame that changes (so a daemon
// window gets it too) and turned off by terminalRestore(). 1000: presses,
// releases and the wheel; 1006: reported as SGR, without X10's 223-column
// limit.
#define MOUSE_ON_SEQ     "\x1b[?1000h\x1b[?1006h"
#define MOUSE_OFF_SEQ    "\x1b[?1006l\x1b[?1000l"
#define MOUSE_SEQ_LEN    16

// A key with modifiers no plain byte can carry: Alt/Option+anything,
// Ctrl+arrows/Home/F-keys/digits/punctuation, Shift+Tab, ... It is one
// Utf8Char of length 1 whose character[0] is MOD_KEY, character[1] the
// KEY_MOD_* bits and character[2] the key without them: a character
// ('x', 'X', '1', ' ', '\r', '\t', ESC, BACKSPACE) or one of the codes
// above (ARROW_LEFT, FUNCTION_KEY + n, ...). keyMake() builds it, and
// keyToString() names it "CTRL-ALT-SHIFT-<key>" ("ALT-x", "CTRL-ARROW-UP",
// "SHIFT-TAB"), modifiers always in that order. Plain Ctrl+letter stays
// its control byte ("CTRL-a"), Shift+letter the capital, and Alt+Shift+
// letter is "ALT-X".
#define MOD_KEY          0xBE
#define KEY_MOD_SHIFT    1
#define KEY_MOD_ALT      2      // Alt, Option, Meta
#define KEY_MOD_CTRL     4
#define IS_MOD_KEY(k)    ((k)->length == 1 && (k)->character[0] == MOD_KEY)
// Asks the terminal to report keys that otherwise all look the same
// (Ctrl+1, Ctrl+., Ctrl+Shift+a, ...) with xterm's modifyOtherKeys, level
// 1: keys that already have a code (Ctrl+a, Alt+x) keep it. Sent with
// every frame like MOUSE_ON_SEQ; terminals without it ignore it.
#define EXTENDED_KEYS_ON_SEQ   "\x1b[>4;1m"
#define EXTENDED_KEYS_OFF_SEQ  "\x1b[>4;0m"
#define EXTENDED_KEYS_SEQ_LEN  7

#define BACKSPACE    127    // 0x7F (DEL byte many terminals send for Backspace)

#define ABUF_INIT {NULL, 0, 0}

#define BELUGA_VERSION "3.4"



#define CSI_MAX_PARAMS 16

#endif // DEFINE_H_
