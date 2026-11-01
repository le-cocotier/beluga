// include/pty_session.h
#ifndef PTY_SESSION_H_
#define PTY_SESSION_H_

#include "data.h"

int ptySessionSpawn(struct buffer_t* buf, const char* shell_path, int rows, int cols);
// The next terminal buffer created (bufferCreate(name, ..., TERMINAL_BUFF))
// runs this program instead of a shell: argv, NULL-terminated; cwd (NULL:
// the window's); env as os_pty_spawn() takes it. The strings are only
// borrowed until then. NULL clears it.
void ptySessionSetNextProgram(const char* const* argv, const char* cwd, const char* const* env);
long ptySessionPid(const struct buffer_t* buf);          // -1 once closed
void ptySessionResize(const struct buffer_t* buf, int rows, int cols);
long ptySessionWrite(const struct buffer_t* buf, const char* bytes, size_t len);
OsStream *ptySessionOutput(const struct buffer_t* buf); // NULL once closed
void ptySessionClose(struct buffer_t* buf);

#endif
