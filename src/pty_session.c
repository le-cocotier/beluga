/**
 * @file pty_session.c
 * @brief Terminal buffers: a shell on a pseudo-terminal (os_pty_*: forkpty
 * on Unix, ConPTY on Windows), its output parsed into the buffer's screen.
 */

#include "../include/pty_session.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/split_screen.h"

#include <string.h>

int ptySessionSpawn(struct buffer_t* buf, const char* shell_path, const int rows, const int cols)
{
    char* cwd = editorGetCwd();
    buf->pty = os_pty_open(shell_path, cwd, rows, cols);
    gfree(cwd);
    if (!buf->pty)
    {
        appDebug("couldn't start a shell for the terminal buffer");
        return -1;
    }

    memset(&buf->term_state, 0, sizeof(TermParserState));
    buf->term_state.state = TPARSE_GROUND;
    buf->term_state.pane_height = splitScreenGetActivePane()->height;
    buf->term_state.pane_width = cols;
    return 0;
}

void ptySessionResize(const struct buffer_t* buf, const int rows, const int cols)
{
    if (buf->pty)
        os_pty_resize(buf->pty, rows, cols);
}

long ptySessionWrite(const struct buffer_t* buf, const char* bytes, const size_t len)
{
    return buf->pty ? os_pty_write(buf->pty, bytes, len) : -1;
}

OsStream *ptySessionOutput(const struct buffer_t* buf)
{
    return buf->pty ? os_pty_output(buf->pty) : NULL;
}

int ptySessionIsAlive(struct buffer_t* buf)
{
    return buf->pty && os_pty_alive(buf->pty);
}

void ptySessionClose(struct buffer_t* buf)
{
    if (buf->pty)
        os_pty_close(buf->pty);
    buf->pty = NULL;
}
