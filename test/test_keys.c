/**
 * @file test_keys.c
 * @brief Unit tests for key decoding (readUtf8Char()) and key names
 */

#include "test.h"
#include "../include/define.h"
#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/utf8.h"

// The keyboard: readUtf8Char() reads from here through screenReadKeys().
static const char *s_input;
static size_t s_input_len;

static long fakeReadKeys(void *buf, const size_t n, const int timeout_ms) {
  const size_t got = n < s_input_len ? n : s_input_len;
  (void)timeout_ms;
  memcpy(buf, s_input, got);
  s_input += got;
  s_input_len -= got;
  return (long)got;
}

static void fakeWrite(const void *buf, const size_t n) {
  (void)buf;
  (void)n;
}

static int fakeSize(int *rows, int *cols) {
  *rows = 24;
  *cols = 80;
  return 0;
}

static const ScreenDriver s_fake = {fakeWrite, fakeReadKeys, fakeSize};

// The name of the key `bytes` decode to; every byte must be used up.
#define CHECK_KEY(bytes, name) checkKey(__LINE__, bytes, sizeof(bytes) - 1, name)

static void checkKey(const int line, const char *bytes, const size_t n, const char *name) {
  Utf8Char *key;
  s_input = bytes;
  s_input_len = n;
  key = readUtf8Char();
  testCheckStr(__FILE__, line, "keyToString(readUtf8Char())", keyToString(key), name);
  if (s_input_len != 0) {
    fprintf(stderr, "%s:%d: %zu byte(s) left over\n", __FILE__, line, s_input_len);
    test_failures++;
  }
  gfree(key);
}

static void testPlainKeys(void) {
  CHECK_KEY("a", "a");
  CHECK_KEY("A", "A");
  CHECK_KEY("\x01", "CTRL-a");
  CHECK_KEY("\x1a", "CTRL-z");
  CHECK_KEY("\x1c", "CTRL-\\");
  CHECK_KEY("\x1d", "CTRL-]");
  CHECK_KEY("\x1f", "CTRL-_");
  CHECK_KEY("\0", "CTRL-SPACE");
  CHECK_KEY("\r", "ENTER");
  CHECK_KEY("\t", "TAB");
  CHECK_KEY("\x7f", "BACKSPACE");
  CHECK_KEY("\x1b", "ESCAPE");
  CHECK_KEY("\xc3\xa9", "\xc3\xa9");
}

// Alt/Option as Meta: ESC, then the key.
static void testAltPrefix(void) {
  CHECK_KEY("\x1bx", "ALT-x");
  CHECK_KEY("\x1bw", "ALT-w");
  CHECK_KEY("\x1bX", "ALT-X");
  CHECK_KEY("\x1b" "1", "ALT-1");
  CHECK_KEY("\x1b.", "ALT-.");
  CHECK_KEY("\x1b{", "ALT-{");
  CHECK_KEY("\x1b ", "ALT-SPACE");
  CHECK_KEY("\x1b\r", "ALT-ENTER");
  CHECK_KEY("\x1b\t", "ALT-TAB");
  CHECK_KEY("\x1b\x7f", "ALT-BACKSPACE");
  CHECK_KEY("\x1b\x01", "CTRL-ALT-a");
  CHECK_KEY("\x1b\0", "CTRL-ALT-SPACE");
  CHECK_KEY("\x1b\x1d", "CTRL-ALT-]");
  CHECK_KEY("\x1b" "f", "ALT-f");
  CHECK_KEY("\x1b" "b", "ALT-b");
  // Option+é: typed.
  CHECK_KEY("\x1b\xc3\xa9", "\xc3\xa9");
  // A whole sequence behind the Meta prefix.
  CHECK_KEY("\x1b\x1b[C", "ALT-ARROW-RIGHT");
  CHECK_KEY("\x1b\x1b[A", "ALT-ARROW-UP");
  CHECK_KEY("\x1b\x1bOP", "ALT-F1");
  CHECK_KEY("\x1b\x1b[15~", "ALT-F5");
}

// xterm's CSI sequences with a modifier parameter.
static void testModifiedSpecialKeys(void) {
  CHECK_KEY("\x1b[A", "ARROW-UP");
  CHECK_KEY("\x1bOA", "ARROW-UP");
  CHECK_KEY("\x1b[1;3C", "ALT-ARROW-RIGHT");
  CHECK_KEY("\x1b[1;3D", "ALT-ARROW-LEFT");
  CHECK_KEY("\x1b[1;9D", "ALT-ARROW-LEFT"); // Meta
  CHECK_KEY("\x1b[1;5A", "CTRL-ARROW-UP");
  CHECK_KEY("\x1b[1;2B", "SHIFT-ARROW-DOWN");
  CHECK_KEY("\x1b[1;6C", "CTRL-SHIFT-ARROW-RIGHT");
  CHECK_KEY("\x1b[1;8D", "CTRL-ALT-SHIFT-ARROW-LEFT");
  CHECK_KEY("\x1b[1;5H", "CTRL-HOME");
  CHECK_KEY("\x1b[1;5F", "CTRL-END");
  CHECK_KEY("\x1b[3;5~", "CTRL-DEL");
  CHECK_KEY("\x1b[3;3~", "ALT-DEL");
  CHECK_KEY("\x1b[5;5~", "CTRL-PAGE-UP");
  CHECK_KEY("\x1b[6;3~", "ALT-PAGE-DOWN");
  CHECK_KEY("\x1b[15~", "F5");
  CHECK_KEY("\x1b[15;2~", "SHIFT-F5");
  CHECK_KEY("\x1b[15;5~", "CTRL-F5");
  CHECK_KEY("\x1b[24;7~", "CTRL-ALT-F12");
  CHECK_KEY("\x1b[1;5P", "CTRL-F1");
  CHECK_KEY("\x1bO5P", "CTRL-F1");
  CHECK_KEY("\x1b[25~", "SHIFT-F5"); // Terminal.app
  CHECK_KEY("\x1b[Z", "SHIFT-TAB");
  CHECK_KEY("\x1b[2~", "KEY-143"); // Insert: UNKNOWN_KEY
  // rxvt.
  CHECK_KEY("\x1bOa", "CTRL-ARROW-UP");
  CHECK_KEY("\x1b[d", "SHIFT-ARROW-LEFT");
  CHECK_KEY("\x1b[3^", "CTRL-DEL");
  CHECK_KEY("\x1b[5$", "SHIFT-PAGE-UP");
}

// modifyOtherKeys (CSI 27 ; m ; c ~) and CSI u (CSI c ; m u).
static void testExtendedReports(void) {
  CHECK_KEY("\x1b[27;5;49~", "CTRL-1");
  CHECK_KEY("\x1b[27;5;46~", "CTRL-.");
  CHECK_KEY("\x1b[27;6;65~", "CTRL-SHIFT-a");
  CHECK_KEY("\x1b[27;5;97~", "CTRL-a");
  CHECK_KEY("\x1b[27;3;97~", "ALT-a");
  CHECK_KEY("\x1b[27;4;97~", "ALT-A");
  CHECK_KEY("\x1b[27;2;13~", "SHIFT-ENTER");
  CHECK_KEY("\x1b[27;5;13~", "CTRL-ENTER");
  CHECK_KEY("\x1b[27;5;9~", "CTRL-TAB");
  CHECK_KEY("\x1b[27;2;33~", "!");
  CHECK_KEY("\x1b[27;5;32~", "CTRL-SPACE");
  CHECK_KEY("\x1b[49;5u", "CTRL-1");
  CHECK_KEY("\x1b[97;6u", "CTRL-SHIFT-a");
  CHECK_KEY("\x1b[127;5u", "CTRL-BACKSPACE");
  CHECK_KEY("\x1b[27u", "ESCAPE");
  CHECK_KEY("\x1b[97:65;6u", "CTRL-SHIFT-a"); // kitty's sub-parameters
  CHECK_KEY("\x1b[233;5u", "\xc3\xa9");
}

static void testCanonicalNames(void) {
  char out[64];
  keyCanonicalName("ALT-x", out, sizeof(out));
  CHECK_STR(out, "ALT-x");
  keyCanonicalName("M-x", out, sizeof(out));
  CHECK_STR(out, "ALT-x");
  keyCanonicalName("OPTION-x", out, sizeof(out));
  CHECK_STR(out, "ALT-x");
  keyCanonicalName("META-ARROW-LEFT", out, sizeof(out));
  CHECK_STR(out, "ALT-ARROW-LEFT");
  keyCanonicalName("SHIFT-ALT-a", out, sizeof(out));
  CHECK_STR(out, "ALT-A");
  keyCanonicalName("ALT-CTRL-a", out, sizeof(out));
  CHECK_STR(out, "CTRL-ALT-a");
  keyCanonicalName("C-M-a", out, sizeof(out));
  CHECK_STR(out, "CTRL-ALT-a");
  keyCanonicalName("CTRL-A", out, sizeof(out));
  CHECK_STR(out, "CTRL-a");
  keyCanonicalName("SHIFT-CTRL-a", out, sizeof(out));
  CHECK_STR(out, "CTRL-SHIFT-a");
  keyCanonicalName("SHIFT-F5", out, sizeof(out));
  CHECK_STR(out, "SHIFT-F5");
  keyCanonicalName("ALT-CTRL-F5", out, sizeof(out));
  CHECK_STR(out, "CTRL-ALT-F5");
  keyCanonicalName("ALT- ", out, sizeof(out));
  CHECK_STR(out, "ALT-SPACE");
  keyCanonicalName("CTRL-SPACE", out, sizeof(out));
  CHECK_STR(out, "CTRL-SPACE");
  keyCanonicalName("ALT--", out, sizeof(out));
  CHECK_STR(out, "ALT--");
  // Not a key with modifiers: untouched.
  keyCanonicalName("-", out, sizeof(out));
  CHECK_STR(out, "-");
  keyCanonicalName("S", out, sizeof(out));
  CHECK_STR(out, "S");
  keyCanonicalName("WHEEL-UP", out, sizeof(out));
  CHECK_STR(out, "WHEEL-UP");
  keyCanonicalName("ENTER", out, sizeof(out));
  CHECK_STR(out, "ENTER");
}

static void testFallback(void) {
  Utf8Char key;
  Utf8Char out;
  keyMake(&key, KEY_MOD_ALT, 'a');
  CHECK(keyFallback(&key, &out));
  CHECK_STR(keyToString(&out), "a");
  keyMake(&key, KEY_MOD_ALT, 'f');
  CHECK(keyFallback(&key, &out));
  CHECK_STR(keyToString(&out), "ALT-ARROW-RIGHT");
  keyMake(&key, KEY_MOD_CTRL, ARROW_UP);
  CHECK(keyFallback(&key, &out));
  CHECK_STR(keyToString(&out), "ARROW-UP");
  keyMake(&key, KEY_MOD_CTRL | KEY_MOD_ALT, 'a');
  CHECK(keyFallback(&key, &out));
  CHECK_STR(keyToString(&out), "CTRL-a");
  keyMake(&key, KEY_MOD_SHIFT, '\t');
  CHECK(keyFallback(&key, &out));
  CHECK_STR(keyToString(&out), "TAB");
  keyMake(&key, KEY_MOD_CTRL, 'a');
  CHECK(!keyFallback(&key, &out));
}

#define CHECK_SEQ(mods, base, expected)                                        \
  do {                                                                         \
    Utf8Char k_;                                                               \
    char seq_[32];                                                             \
    int n_;                                                                    \
    keyMake(&k_, mods, base);                                                  \
    n_ = keyEscapeSequence(&k_, seq_, sizeof(seq_));                           \
    CHECK_INT(n_, sizeof(expected) - 1);                                       \
    CHECK(memcmp(seq_, expected, sizeof(expected) - 1) == 0);                  \
  } while (0)

// What a terminal pane's program gets.
static void testEscapeSequences(void) {
  CHECK_SEQ(0, 'a', "a");
  CHECK_SEQ(KEY_MOD_CTRL, 'a', "\x01");
  CHECK_SEQ(KEY_MOD_ALT, 'f', "\x1b" "f");
  CHECK_SEQ(KEY_MOD_ALT | KEY_MOD_SHIFT, 'f', "\x1b" "F");
  CHECK_SEQ(KEY_MOD_CTRL | KEY_MOD_ALT, 'a', "\x1b\x01");
  CHECK_SEQ(KEY_MOD_ALT, '\r', "\x1b\r");
  CHECK_SEQ(KEY_MOD_ALT, BACKSPACE, "\x1b\x7f");
  CHECK_SEQ(0, ARROW_UP, "\x1b[A");
  CHECK_SEQ(KEY_MOD_ALT, ARROW_LEFT, "\x1b[1;3D");
  CHECK_SEQ(KEY_MOD_CTRL, DEL_KEY, "\x1b[3;5~");
  CHECK_SEQ(0, FUNCTION_KEY, "\x1bOP");
  CHECK_SEQ(KEY_MOD_SHIFT, FUNCTION_KEY + 4, "\x1b[15;2~");
  CHECK_SEQ(KEY_MOD_SHIFT, FUNCTION_KEY, "\x1b[1;2P");
  CHECK_SEQ(KEY_MOD_SHIFT, '\t', "\x1b[Z");
  CHECK_SEQ(KEY_MOD_CTRL, '1', "\x1b[27;5;49~");
}

int main(void) {
  testInit();
  screenSetDriver(&s_fake);
  testPlainKeys();
  testAltPrefix();
  testModifiedSpecialKeys();
  testExtendedReports();
  testCanonicalNames();
  testFallback();
  testEscapeSequences();
  return testDone();
}
