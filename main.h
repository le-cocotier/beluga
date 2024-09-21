#ifndef MAIN_H
#define MAIN_H
/* terminal */

void die(const char *s);
void disableRawMode();
void enableRawMode();
char editorReadKey();

/* output */
void wipeScreen();

/* input */
void editorProcessKeypress();
#endif
