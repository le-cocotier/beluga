/**
 * @file test_macro.c
 * @brief Unit tests for keyboard macro recording (macro.c): which keys a
 * macro keeps, and the status messages for the cases that keep none.
 * Replay drives editorProcessKeypress(), which the pty tests cover.
 */

#include "test.h"
#include "../include/galloc.h"
#include "../include/macro.h"
#include "../include/utf8.h"

static Utf8Char key(const char *s) {
  Utf8Char k;
  memset(&k, 0, sizeof(k));
  utf8CreateChar((const unsigned char *)s, &k);
  return k;
}

// A key read at the start of a key sequence (no prefix pending).
static void type(const char *s) {
  Utf8Char k = key(s);
  E.prefix_state = 0;
  macroNoteKeySequenceStart();
  macroRecordKey(&k);
}

// The keys of the stopping command: CTRL-x, then ")" under the prefix.
static void typeStop(void) {
  Utf8Char ctrl_x = key("\x18");
  Utf8Char paren = key(")");
  E.prefix_state = 0;
  macroNoteKeySequenceStart();
  macroRecordKey(&ctrl_x);
  E.prefix_state = 1;
  macroNoteKeySequenceStart(); // inside the sequence: doesn't move the mark
  macroRecordKey(&paren);
  E.prefix_state = 0;
  macroStopRecording();
}

static int statusContains(const char *needle) {
  char *msg = (char *)utf8ToString(E.status_msg);
  int found = msg && strstr(msg, needle) != NULL;
  if (!found)
    fprintf(stderr, "status is \"%s\", expected it to contain \"%s\"\n",
            msg ? msg : "(null)", needle);
  gfree(msg);
  return found;
}

static void testRecord(void) {
  macroStartRecording();
  CHECK(macroIsRecording());
  CHECK(statusContains("Defining macro"));
  type("a");
  type("\xc3\xa9"); // é: whole UTF-8 characters are kept
  type("c");
  typeStop();
  CHECK(!macroIsRecording());
  // The CTRL-x ) that stopped it isn't part of the macro.
  CHECK(statusContains("Macro defined (3 keys)"));
}

static void testOneKey(void) {
  macroStartRecording();
  type("z");
  typeStop();
  CHECK(statusContains("Macro defined (1 key)"));
}

static void testEmptyKeepsPrevious(void) {
  macroStartRecording();
  typeStop();
  CHECK(statusContains("Empty macro, kept the previous one"));
  CHECK(!macroIsRecording());
}

static void testMisuse(void) {
  Utf8Char k = key("q");
  Utf8Char none;

  // Keys aren't recorded while not recording.
  macroRecordKey(&k);
  macroStopRecording();
  CHECK(statusContains("Not defining a macro"));

  macroStartRecording();
  macroStartRecording();
  CHECK(statusContains("Already defining a macro"));
  CHECK(macroIsRecording());
  macroRun(1);
  CHECK(statusContains("Can't run a macro while defining one"));
  // NULL and empty keys are ignored.
  memset(&none, 0, sizeof(none));
  macroRecordKey(NULL);
  macroRecordKey(&none);
  type("x");
  type("y");
  typeStop();
  CHECK(statusContains("Macro defined (2 keys)"));

  // Nothing to hand out unless a macro is being replayed.
  CHECK(macroNextReplayedKey() == NULL);
}

static void testRunWithoutMacro(void) {
  macroFree();
  macroRun(3);
  CHECK(statusContains("No macro defined"));
}

int main(void) {
  testInit();
  testRecord();
  testOneKey();
  testEmptyKeepsPrevious();
  testMisuse();
  testRunWithoutMacro();
  macroFree();
  return testDone();
}
