/**
 * @file macro.c
 * @brief Keyboard macros: record keys, replay them
 * @details The recording hooks into editorReadKey(), the one place every
 * key goes through, so prompts answered while recording (a search string,
 * a file name) are replayed too. Replay pushes the keys into a queue that
 * editorReadKey() drains before reading the terminal, and runs
 * editorProcessKeypress() until the queue is empty.
 */

#include "../include/macro.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/input.h"

#include <string.h>

extern EditorConfig E;

// A macro longer than this is almost certainly a recording left running.
#define MACRO_MAX_KEYS 100000

static Utf8Char *s_keys = NULL; // the last complete macro
static int s_count = 0;

static Utf8Char *s_recording = NULL; // the one being recorded
static int s_rec_count = 0;
static int s_rec_cap = 0;
static int s_is_recording = 0;
static int s_sequence_start = 0; // s_rec_count when the current key sequence began

static const Utf8Char *s_replay = NULL; // keys being replayed, and how far
static int s_replay_len = 0;
static int s_replay_pos = 0;
static int s_replaying = 0;

int macroIsRecording(void) { return s_is_recording; }

void macroStartRecording(void) {
  if (s_replaying)
    return;
  if (s_is_recording) {
    editorSetStatusMessage("Already defining a macro");
    return;
  }
  s_rec_count = 0;
  s_sequence_start = 0;
  s_is_recording = 1;
  editorSetStatusMessage("Defining macro... (CTRL-x ) to finish)");
}

void macroStopRecording(void) {
  if (s_replaying)
    return;
  if (!s_is_recording) {
    editorSetStatusMessage("Not defining a macro");
    return;
  }
  s_is_recording = 0;
  // Drop the keys that ran this command.
  s_rec_count = s_sequence_start;
  if (s_rec_count == 0) {
    editorSetStatusMessage("Empty macro, kept the previous one");
    return;
  }
  gfree(s_keys);
  s_keys = s_recording;
  s_count = s_rec_count;
  s_recording = NULL;
  s_rec_count = 0;
  s_rec_cap = 0;
  editorSetStatusMessage("Macro defined (%d key%s) - CTRL-x x to run it",
                         s_count, s_count == 1 ? "" : "s");
}

void macroNoteKeySequenceStart(void) {
  // A key sequence starts when no prefix is pending; inside one (after
  // CTRL-x), the keys still belong to the sequence that began earlier.
  if (s_is_recording && E.prefix_state == 0)
    s_sequence_start = s_rec_count;
}

void macroRecordKey(const Utf8Char *key) {
  Utf8Char *grown;
  int cap;
  if (!s_is_recording || !key || key->character[0] == 0)
    return;
  if (s_rec_count >= MACRO_MAX_KEYS) {
    s_is_recording = 0;
    editorSetStatusMessage("Macro too long, recording stopped");
    return;
  }
  if (s_rec_count >= s_rec_cap) {
    cap = s_rec_cap ? s_rec_cap * 2 : 64;
    grown = grealloc(s_recording, (size_t)cap * sizeof(Utf8Char));
    if (!grown)
      return;
    s_recording = grown;
    s_rec_cap = cap;
  }
  s_recording[s_rec_count++] = *key;
}

Utf8Char *macroNextReplayedKey(void) {
  Utf8Char *key;
  if (!s_replaying || s_replay_pos >= s_replay_len)
    return NULL;
  key = gcalloc(1, sizeof(Utf8Char));
  if (key)
    *key = s_replay[s_replay_pos++];
  return key;
}

void macroRun(int times) {
  int i;
  if (s_replaying)
    return; // a macro that runs itself would never end
  if (s_is_recording) {
    editorSetStatusMessage("Can't run a macro while defining one");
    return;
  }
  if (s_count == 0) {
    editorSetStatusMessage("No macro defined (CTRL-x ( to start one)");
    return;
  }
  if (times < 1)
    times = 1;

  // CTRL-x x itself left the prefix pending; the macro starts from none.
  E.prefix_state = 0;
  s_replaying = 1;
  s_replay = s_keys;
  s_replay_len = s_count;
  for (i = 0; i < times && !E.exit_requested; i++) {
    s_replay_pos = 0;
    while (s_replay_pos < s_replay_len && !E.exit_requested)
      editorProcessKeypress();
  }
  s_replaying = 0;
  s_replay = NULL;
  s_replay_len = 0;
  s_replay_pos = 0;
}

void macroFree(void) {
  gfree(s_keys);
  gfree(s_recording);
  s_keys = NULL;
  s_recording = NULL;
  s_count = 0;
  s_rec_count = 0;
  s_rec_cap = 0;
}
