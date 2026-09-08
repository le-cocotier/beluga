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
static void killAndReap(pid_t pid)
{
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

char *runCommandCapture(const char *cmd, long timeout_ms, size_t max_bytes)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    pid_t pid = fork();
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
    setpgid(pid, pid); // mirrors the child's own call — avoids the race
                       // where the parent's kill(-pid, ...) below could
                       // run before the child finishes joining its group
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK); // pipe fds start with flags 0,
                                            // so this can't clobber anything

    size_t cap = 256, len = 0;
    char* out = malloc(cap);
    if (!out)
    {
        close(pipefd[0]);
        killAndReap(pid);
        return NULL;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000L)   // keep it a well-formed timespec:
    {                                      // the remaining_ms sum below is
        deadline.tv_sec += 1;              // linear and comes out the same
        deadline.tv_nsec -= 1000000000L;   // either way, but tv_nsec >= 1e9 is
    }                                      // malformed and invites a later bug

    int timed_out = 0, capped = 0;
    for (;;)
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                            (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remaining_ms <= 0) { timed_out = 1; break; }

        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, (int) remaining_ms);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) { timed_out = 1; break; } // deadline reached

        char chunk[512];
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
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
            char* grown = realloc(out, cap);
            if (!grown)
            {
                free(out);
                close(pipefd[0]);
                killAndReap(pid);
                return NULL;
            }
            out = grown;
        }
        memcpy(out + len, chunk, (size_t) n);
        len += (size_t) n;

        if (capped) break;
    }

    close(pipefd[0]);
    if (timed_out || capped)
        kill(-pid, SIGKILL); // whole process group: sh and what it spawned
    waitpid(pid, NULL, 0);

    out[len] = '\0';
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';

    if (len == 0) { free(out); return NULL; }
    return out;
}

void shellQuote(const char *s, char *out, size_t out_size)
{
    size_t pos = 0;
    if (pos + 1 < out_size) out[pos++] = '\'';
    for (const char* p = s; *p && pos + 5 < out_size; p++)
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
