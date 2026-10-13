#ifndef REPLACE_H_
#define REPLACE_H_

// Query-replace (replace.c): prompts for the text and its replacement,
// then walks the matches from the cursor to the end of the buffer, asking
// at each one (y/n, ! for the rest, q to stop). Undoes as one step.
void queryReplacePrompt(void);

// The same over every file of the project containing the text (found the
// way project-search finds it), opening them as needed. A for "all files"
// replaces everything left without asking. Buffers are left unsaved.
void projectQueryReplacePrompt(void);

#endif // REPLACE_H_
