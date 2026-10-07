// include/pty_session.h
#ifndef PTY_SESSION_H_
#define PTY_SESSION_H_

#include "data.h"

int ptySessionSpawn(struct buffer_t* buf, const char* shell_path, int rows, int cols);
void ptySessionResize(const struct buffer_t* buf, int rows, int cols);
long ptySessionWrite(const struct buffer_t* buf, const char* bytes, size_t len);
OsStream *ptySessionOutput(const struct buffer_t* buf); // NULL once closed
int ptySessionIsAlive(struct buffer_t* buf);            // 0 once the shell has exited
void ptySessionClose(struct buffer_t* buf);

#endif
