/**
 * @file subprocess.c
 * @brief One hardened "run a command, capture stdout" helper, shared by
 * the git integration and project-wide search.
 * @details Extracted from buffer.c, where it started life as the git
 * integration's private gitRunCommand(). Project search needs exactly the
 * same guarantees — a deadline, a killable process group, non-blocking
 * reads — so the choice was to share this or to grow a second copy of
 * fork/pipe/poll/timeout code. The only things that differ per caller are
 * the deadline and how much output is worth keeping, so both became
 * parameters.
 */

#include "../include/subprocess.h"
#include "../include/galloc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Kills the whole process group (the command and the sh that
 * spawned it) and reaps it, so runCommandCapture()'s early-return paths
 * don't each repeat the same three lines.
 */
static void killAndReap(const pid_t pid)
{
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/**
 * @brief Reads a just-forked child's output pipe until EOF, the deadline,
 * or the byte cap, then reaps the child — the part runCommandCapture() and
 * runArgvCapture() share; they differ only in how the child is exec'd.
 * @param pid The child, already in its own process group.
 * @param readfd Read end of the child's output pipe; closed here.
 * @param exit_status If non-NULL, receives the child's exit code, or -1 if
 * it was killed (deadline, cap) or didn't exit normally.
 * @return The output malloc'd and NUL-terminated with trailing newlines
 * stripped, or NULL if there was none.
 */
static char *captureChild(const pid_t pid, const int readfd, const long timeout_ms,
                          const size_t max_bytes, int *exit_status)
{
    size_t cap;
    size_t len;
    char* out;
    struct timespec deadline;
    int timed_out;
    int capped;
    struct timespec now;
    long remaining_ms;
    struct pollfd pfd;
    int pr;
    char chunk[512];
    ssize_t n;
    char* grown;
    int status;

    if (exit_status) *exit_status = -1;
    setpgid(pid, pid); // mirrors the child's own call — avoids the race
                       // where the parent's kill(-pid, ...) below could
                       // run before the child finishes joining its group
    fcntl(readfd, F_SETFL, O_NONBLOCK); // pipe fds start with flags 0,
                                         // so this can't clobber anything

    cap = 256; len = 0;
    out = galloc(cap);
    if (!out)
    {
        close(readfd);
        killAndReap(pid);
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000L)   // keep it a well-formed timespec:
    {                                      // the remaining_ms sum below is
        deadline.tv_sec += 1;              // linear and comes out the same
        deadline.tv_nsec -= 1000000000L;   // either way, but tv_nsec >= 1e9 is
    }                                      // malformed and invites a later bug

    timed_out = 0; capped = 0;
    for (;;)
    {
        clock_gettime(CLOCK_MONOTONIC, &now);
        remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                            (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remaining_ms <= 0) { timed_out = 1; break; }

        pfd = (struct pollfd){ .fd = readfd, .events = POLLIN };
        pr = poll(&pfd, 1, (int) remaining_ms);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) { timed_out = 1; break; } // deadline reached

        n = read(readfd, chunk, sizeof(chunk));
        if (n <= 0)
        {
            if (n < 0 && errno == EAGAIN) continue;
            break; // EOF or a real read error — command is done
        }

        if (max_bytes > 0 && len + (size_t) n > max_bytes)
        {
            // Keep what still fits under the cap, then stop: a search for
            // "e" across a large tree would otherwise happily grow this
            // buffer until the machine complained.
            n = (ssize_t) (max_bytes - len);
            capped = 1;
        }

        if (len + (size_t) n + 1 > cap)
        {
            while (len + (size_t) n + 1 > cap) cap *= 2;
            grown = grealloc(out, cap);
            if (!grown)
            {
                gfree(out);
                close(readfd);
                killAndReap(pid);
                return NULL;
            }
            out = grown;
        }
        memcpy(out + len, chunk, (size_t) n);
        len += (size_t) n;

        if (capped) break;
    }

    close(readfd);
    if (timed_out || capped)
        kill(-pid, SIGKILL); // whole process group: sh and what it spawned
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status) && exit_status)
        *exit_status = WEXITSTATUS(status);

    out[len] = '\0';
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';

    if (len == 0) { gfree(out); return NULL; }
    return out;
}

char *runCommandCapture(const char *cmd, const long timeout_ms, const size_t max_bytes)
{
    int pipefd[2];
    pid_t pid;
    if (pipe(pipefd) != 0) return NULL;

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0)
    {
        // Child: stdout -> pipe write end. stderr is left alone — callers
        // redirect it inside `cmd` with "2>/dev/null" when they don't want
        // it. Its own process group lets the parent kill the whole sh+child
        // tree on timeout, not just the immediate child.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char*) NULL);
        _exit(127);
    }

    close(pipefd[1]);
    return captureChild(pid, pipefd[0], timeout_ms, max_bytes, NULL);
}

char *runArgvCapture(char *const argv[], const long timeout_ms, const size_t max_bytes,
                     int *exit_status)
{
    int pipefd[2];
    pid_t pid;
    int devnull;
    if (exit_status) *exit_status = -1;
    if (!argv || !argv[0] || pipe(pipefd) != 0) return NULL;

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0)
    {
        // Child: stdout AND stderr -> pipe, so a failing mv/cp can say why.
        // stdin from /dev/null: the editor owns the terminal in raw mode,
        // and anything that tried to prompt (rm -i, an overwrite question)
        // would otherwise read the user's next keystrokes.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        setpgid(0, 0);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    return captureChild(pid, pipefd[0], timeout_ms, max_bytes, exit_status);
}

void shellQuote(const char *s, char *out, const size_t out_size)
{
    size_t pos = 0;
    const char* p;
    if (pos + 1 < out_size) out[pos++] = '\'';
    for (p = s; *p && pos + 5 < out_size; p++)
    {
        if (*p == '\'')
        {
            memcpy(out + pos, "'\\''", 4);
            pos += 4;
        }
        else
        {
            out[pos++] = *p;
        }
    }
    if (pos + 1 < out_size) out[pos++] = '\'';
    out[pos] = '\0';
}
