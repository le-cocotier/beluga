
#include "../include/pty_session.h"
#include "../include/terminal.h"
#include "include/split_screen.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <util.h>       // forkpty — macOS/BSD. On Linux: #include <pty.h> instead.
#else
#include <pty.h>
#endif

int ptySessionSpawn(struct buffer_t* buf, const char* shell_path, int rows, int cols)
{
    struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = 0, .ws_ypixel = 0 };

    pid_t pid = forkpty(&buf->pty_master_fd, NULL, NULL, &ws);
    if (pid < 0)
    {
        appDebug("forkpty failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        // Child process: become the shell
        const char* shell = shell_path ? shell_path : getenv("SHELL");
        if (!shell) shell = "/bin/zsh";
        setenv("TERM", "xterm-256color", 1);
        execl(shell, shell, "-i", (char*) NULL);
        _exit(127); // execl only returns on failure
    }

    // Parent process
    buf->child_pid = pid;

    int flags = fcntl(buf->pty_master_fd, F_GETFL, 0);
    fcntl(buf->pty_master_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&buf->term_state, 0, sizeof(TermParserState));
    buf->term_state.state = TPARSE_GROUND;
    buf->term_state.pane_height = splitScreenGetActivePane()->height;

    return 0;
}

void ptySessionResize(struct buffer_t* buf, int rows, int cols)
{
    if (buf->pty_master_fd < 0) return;
    struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = 0, .ws_ypixel = 0 };
    ioctl(buf->pty_master_fd, TIOCSWINSZ, &ws);
    if (buf->child_pid > 0)
        kill(buf->child_pid, SIGWINCH);
}

ssize_t ptySessionWrite(struct buffer_t* buf, const char* bytes, size_t len)
{
    if (buf->pty_master_fd < 0) return -1;
    return write(buf->pty_master_fd, bytes, len);
}

int ptySessionIsAlive(struct buffer_t* buf)
{
    if (buf->child_pid <= 0) return 0;

    int status;
    pid_t r = waitpid(buf->child_pid, &status, WNOHANG);
    if (r == 0) return 1;   // still running
    buf->child_pid = -1;    // exited (r == pid) or error (r == -1) — either way, done
    return 0;
}

void ptySessionClose(struct buffer_t* buf)
{
    if (buf->child_pid > 0)
    {
        kill(buf->child_pid, SIGHUP);
        waitpid(buf->child_pid, NULL, 0);
        buf->child_pid = -1;
    }
    if (buf->pty_master_fd >= 0)
    {
        close(buf->pty_master_fd);
        buf->pty_master_fd = -1;
    }
}
