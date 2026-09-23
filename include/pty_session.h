// include/pty_session.h
#ifndef PTY_SESSION_H_
#define PTY_SESSION_H_
#include <signal.h>

#include "data.h"

int ptySessionSpawn(struct buffer_t* buf, const char* shell_path, int rows, int cols);
void ptySessionResize(const struct buffer_t* buf, int rows, int cols);
ssize_t ptySessionWrite(const struct buffer_t* buf, const char* bytes, size_t len);
int ptySessionIsAlive(struct buffer_t* buf); // reaps if exited; returns 0 if dead
void ptySessionClose(struct buffer_t* buf);

#endif
