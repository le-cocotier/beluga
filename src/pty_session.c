/**
 * @file pty_session.c
 * @brief Terminal buffers: a shell on a pseudo-terminal (os_pty_*: forkpty
 * on Unix, ConPTY on Windows), its output parsed into the buffer's screen.
 */

#include "../include/pty_session.h"
#include "../include/editor_op.h"
#include "../include/project.h"
#include "../include/galloc.h"
#include "../include/terminal.h"
#include "../include/split_screen.h"

#include <string.h>

static const char* const* s_next_argv;
static const char* s_next_cwd;
static const char* const* s_next_env;

void ptySessionSetNextProgram(const char* const* argv, const char* cwd, const char* const* env)
{
    s_next_argv = argv;
    s_next_cwd = cwd;
    s_next_env = env;
}

int ptySessionSpawn(struct buffer_t* buf, const char* shell_path, const int rows, const int cols)
{
    // The current project's root when there is one, like find-file.
    char* cwd = projectRootOrCwd();
    if (s_next_argv)
        buf->pty = os_pty_spawn(s_next_argv, s_next_cwd ? s_next_cwd : cwd, s_next_env, rows, cols);
    else
        buf->pty = os_pty_open(shell_path, cwd, rows, cols);
    ptySessionSetNextProgram(NULL, NULL, NULL);
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

long ptySessionPid(const struct buffer_t* buf)
{
    return buf->pty ? os_pty_pid(buf->pty) : -1;
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
