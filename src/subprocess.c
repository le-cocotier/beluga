/**
 * @file subprocess.c
 * @brief Run a program, capture its output: the one hardened helper behind
 * process-run, shell-run, the git integration, project search and the
 * file finder.
 * @details Everything runs synchronously on the editor's own thread, so no
 * call may block on a slow or hung program: each has a deadline (the whole
 * process tree is killed when it passes) and an output cap. Programs are
 * always started from an argv — never a shell command string — through
 * os.h, so the same code runs on every platform.
 */

#include "../include/subprocess.h"
#include "../include/galloc.h"
#include "../include/os.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Runs `spawn` to completion (or its deadline) and returns what it
 * wrote: galloc'd and NUL-terminated, trailing newlines stripped, or NULL
 * if there was none.
 * @param exit_status If non-NULL: the exit code; 127 if the program
 * couldn't be found; -1 if it was killed (deadline, cap) or by a signal.
 */
static char *capture(const OsSpawn *spawn, const long timeout_ms, const size_t max_bytes,
                     int *exit_status, size_t *out_len)
{
    const long long deadline = os_monotonic_ms() + timeout_ms;
    OsProc *p;
    OsStream *out_stream;
    size_t cap = 256;
    size_t len = 0;
    char *out;
    char *grown;
    char chunk[4096];
    long long remaining;
    long n;
    int code = -1;
    int stopped = 0;

    if (exit_status)
        *exit_status = -1;
    if (out_len)
        *out_len = 0;
    p = os_proc_spawn(spawn);
    if (!p)
        return NULL;
    out = galloc(cap);
    if (!out)
    {
        os_proc_free(p);
        return NULL;
    }
    out_stream = os_proc_output(p);

    for (;;)
    {
        remaining = deadline - os_monotonic_ms();
        if (remaining <= 0)
        {
            stopped = 1; // deadline reached
            break;
        }
        n = os_stream_read(out_stream, chunk, sizeof(chunk), (int) (remaining > 1000 ? 1000 : remaining));
        if (n < 0)
            break; // EOF: the program is done writing
        if (n == 0)
            continue;

        if (max_bytes > 0 && len + (size_t) n > max_bytes)
        {
            // Keep what still fits under the cap, then stop: a search for
            // "e" across a large tree would otherwise grow this until the
            // machine complained.
            n = (long) (max_bytes - len);
            stopped = 1;
        }
        if (len + (size_t) n + 1 > cap)
        {
            while (len + (size_t) n + 1 > cap)
                cap *= 2;
            grown = grealloc(out, cap);
            if (!grown)
            {
                stopped = 1;
                break;
            }
            out = grown;
        }
        memcpy(out + len, chunk, (size_t) n);
        len += (size_t) n;
        if (stopped)
            break;
    }

    if (stopped)
        os_proc_kill(p);
    if (os_proc_wait(p, stopped ? -1 : (int) (timeout_ms > 0 ? timeout_ms : 0), &code) && !stopped &&
        exit_status)
        *exit_status = code;
    os_proc_free(p);

    out[len] = '\0';
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    if (out_len)
        *out_len = len;
    if (len == 0)
    {
        gfree(out);
        return NULL;
    }
    return out;
}

/**
 * @brief The OsSpawn every helper here uses: no stdin, and no way to
 * prompt (a password prompt fails instead of waiting for input that will
 * never come).
 */
static OsSpawn spawnFor(char *const argv[], const char *cwd, const OsStderrMode stderr_mode)
{
    OsSpawn spawn;
    memset(&spawn, 0, sizeof(spawn));
    spawn.argv = (const char *const *) argv;
    spawn.cwd = cwd;
    spawn.stdin_pipe = 0;
    spawn.stderr_mode = stderr_mode;
    spawn.no_prompt = 1;
    return spawn;
}

char *runArgvCapture(char *const argv[], const long timeout_ms, const size_t max_bytes,
                     int *exit_status)
{
    return runArgvCaptureIn(argv, NULL, timeout_ms, max_bytes, exit_status);
}

char *runArgvCaptureIn(char *const argv[], const char *cwd, const long timeout_ms,
                       const size_t max_bytes, int *exit_status)
{
    OsSpawn spawn;
    if (exit_status)
        *exit_status = -1;
    if (!argv || !argv[0])
        return NULL;
    spawn = spawnFor(argv, cwd, OS_STDERR_MERGE);
    return capture(&spawn, timeout_ms, max_bytes, exit_status, NULL);
}

char *runArgvCaptureQuiet(char *const argv[], const long timeout_ms, const size_t max_bytes,
                          int *exit_status)
{
    OsSpawn spawn;
    if (exit_status)
        *exit_status = -1;
    if (!argv || !argv[0])
        return NULL;
    spawn = spawnFor(argv, NULL, OS_STDERR_DISCARD);
    return capture(&spawn, timeout_ms, max_bytes, exit_status, NULL);
}

char *runArgvCaptureBytes(char *const argv[], const long timeout_ms, const size_t max_bytes,
                          size_t *len)
{
    OsSpawn spawn;
    if (len)
        *len = 0;
    if (!argv || !argv[0])
        return NULL;
    spawn = spawnFor(argv, NULL, OS_STDERR_DISCARD);
    return capture(&spawn, timeout_ms, max_bytes, NULL, len);
}
