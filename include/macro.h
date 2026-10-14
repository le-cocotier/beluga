#ifndef MACRO_H_
#define MACRO_H_

#include "data.h"

// Keyboard macros (macro.c). Recording captures every key editorReadKey()
// returns — commands, typed text, and answers to prompts alike — and
// replaying feeds them back through the same path, so a macro does
// exactly what the keys did.

void macroStartRecording(void);
// Stops recording; the keys of the command that stopped it (CTRL-x ")")
// aren't part of the macro.
void macroStopRecording(void);
// Replays the last macro `times` times (at least once).
void macroRun(int times);
int macroIsRecording(void);

// Called by editorProcessKeypress() before it reads a key: marks where a
// key sequence starts, so macroStopRecording() knows what to drop.
void macroNoteKeySequenceStart(void);
// editorReadKey()'s side: the next replayed key, if one is pending (NULL
// otherwise), and recording what was read from the terminal.
Utf8Char *macroNextReplayedKey(void);
void macroRecordKey(const Utf8Char *key);
// Frees the recorded keys (at exit).
void macroFree(void);

#endif // MACRO_H_
