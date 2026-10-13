/**
 * @file os_win32.c
 * @brief libbeluga-os-win32: include/os.h for Windows 10 (1809+) and 11.
 * @details How the os.h model maps onto Windows:
 * - Terminal: the console in VT mode. Output escape sequences are
 *   interpreted natively (ENABLE_VIRTUAL_TERMINAL_PROCESSING); keys are
 *   read as console events and turned into the byte sequences an xterm
 *   sends, so the editor's key parser is the same as on Unix.
 * - Streams: the console, or anonymous pipes. Pipes can't be waited on,
 *   only peeked, so os_wait() waits on the console in short slices and
 *   peeks the pipes in between.
 * - Programs: CreateProcess, each in a Job object (os_proc_kill() ends the
 *   whole tree), found on PATH with PATHEXT (npm's .cmd launchers run
 *   through cmd.exe), with a hidden console of their own.
 * - Terminal panes: ConPTY.
 * - Paths are handed to the editor with forward slashes. The executable
 *   embeds a manifest setting the process code page to UTF-8, so the C
 *   runtime's fopen() takes the same UTF-8 paths as on Unix; this file
 *   uses the wide (UTF-16) API throughout.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006 // Windows 10 1809: ConPTY
#endif

#include "../../include/os.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/***********************************************************************************************************************
 *
 *                                                 UTF-8 <-> UTF-16
 *
 **********************************************************************************************************************/

/**
 * @brief UTF-8 to a malloc'd UTF-16 string, or NULL.
 */
static wchar_t *toWide(const char *s)
{
    int n;
    wchar_t *w;
    if (!s)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = malloc((size_t) n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0)
    {
        free(w);
        return NULL;
    }
    return w;
}

/**
 * @brief UTF-16 to a malloc'd UTF-8 string, or NULL.
 */
static char *toUtf8(const wchar_t *w)
{
    int n;
    char *s;
    if (!w)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    s = malloc((size_t) n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0)
    {
        free(s);
        return NULL;
    }
    return s;
}

static void backslashesToSlashes(char *s)
{
    for (; s && *s; s++)
        if (*s == '\\')
            *s = '/';
}

/***********************************************************************************************************************
 *
 *                                                 Process lifetime and identity
 *
 **********************************************************************************************************************/

static UINT s_orig_output_cp;

static void restoreConsoleCodePage(void)
{
    if (s_orig_output_cp)
        SetConsoleOutputCP(s_orig_output_cp);
}

void os_init(void)
{
    // Binary mode everywhere: the editor decides line endings itself
    // (CRLF files are saved as CRLF), so the C runtime mustn't turn each
    // "\n" into "\r\n" behind its back.
    _fmode = _O_BINARY;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    // The UI is written as UTF-8.
    s_orig_output_cp = GetConsoleOutputCP();
    if (s_orig_output_cp && s_orig_output_cp != CP_UTF8)
    {
        SetConsoleOutputCP(CP_UTF8);
        atexit(restoreConsoleCodePage);
    }
}

const char *os_name(void)
{
    return "windows";
}

long os_getpid(void)
{
    return (long) GetCurrentProcessId();
}

/***********************************************************************************************************************
 *
 *                                                 Time
 *
 **********************************************************************************************************************/

long long os_monotonic_ms(void)
{
    return (long long) GetTickCount64();
}

void os_sleep_ms(const int ms)
{
    Sleep(ms > 0 ? (DWORD) ms : 0);
}

/***********************************************************************************************************************
 *
 *                                                 Command lines
 *
 **********************************************************************************************************************/

// ─── Command lines ──────────────────────────────────────────────────────────

typedef struct {
    wchar_t *s;
    size_t len;
    size_t cap;
} WBuf;

static int wbufAppendN(WBuf *b, const wchar_t *s, const size_t n)
{
    wchar_t *grown;
    size_t cap;
    if (b->len + n + 1 > b->cap)
    {
        cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap)
            cap *= 2;
        grown = realloc(b->s, cap * sizeof(wchar_t));
        if (!grown)
            return 0;
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->s[b->len] = L'\0';
    return 1;
}

static int wbufAppend(WBuf *b, const wchar_t *s)
{
    return wbufAppendN(b, s, wcslen(s));
}

static int wbufAppendRepeat(WBuf *b, const wchar_t c, size_t count)
{
    while (count-- > 0)
        if (!wbufAppendN(b, &c, 1))
            return 0;
    return 1;
}

/**
 * @brief Appends one argument quoted so CommandLineToArgvW() (and every
 * program using the Microsoft C runtime's rules) reads it back verbatim.
 * @param force Quote even when there's nothing to protect — needed for
 * anything cmd.exe will see, so its metacharacters stay literal.
 */
static int appendQuotedArg(WBuf *b, const wchar_t *arg, const int force)
{
    const wchar_t *p;
    size_t backslashes;

    if (!force && *arg && !wcspbrk(arg, L" \t\n\v\""))
        return wbufAppend(b, arg);

    if (!wbufAppend(b, L"\""))
        return 0;
    for (p = arg;; p++)
    {
        backslashes = 0;
        while (*p == L'\\')
        {
            p++;
            backslashes++;
        }
        if (*p == L'\0')
        {
            // Backslashes before the closing quote must all be doubled.
            if (!wbufAppendRepeat(b, L'\\', backslashes * 2))
                return 0;
            break;
        }
        if (*p == L'"')
        {
            if (!wbufAppendRepeat(b, L'\\', backslashes * 2 + 1) || !wbufAppendN(b, p, 1))
                return 0;
        }
        else if (!wbufAppendRepeat(b, L'\\', backslashes) || !wbufAppendN(b, p, 1))
            return 0;
    }
    return wbufAppend(b, L"\"");
}

/**
 * @brief Is there a file (not a directory) at dir + name + ext? If so,
 * writes that path to `out`.
 */
static int tryCandidate(const wchar_t *dir, const size_t dir_len, const wchar_t *name,
                        const wchar_t *ext, const size_t ext_len, wchar_t *out, const size_t out_len)
{
    const size_t name_len = wcslen(name);
    const int need_sep = dir_len > 0 && dir[dir_len - 1] != L'\\' && dir[dir_len - 1] != L'/';
    size_t pos = 0;
    DWORD attrs;

    if (dir_len + 1 + name_len + ext_len + 1 > out_len)
        return 0;
    memcpy(out, dir, dir_len * sizeof(wchar_t));
    pos = dir_len;
    if (need_sep)
        out[pos++] = L'\\';
    memcpy(out + pos, name, name_len * sizeof(wchar_t));
    pos += name_len;
    memcpy(out + pos, ext, ext_len * sizeof(wchar_t));
    pos += ext_len;
    out[pos] = L'\0';

    attrs = GetFileAttributesW(out);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/**
 * @brief Tries `name` in one directory: as given if it already has an
 * extension, then with each PATHEXT extension.
 */
static int tryDirectory(const wchar_t *dir, const size_t dir_len, const wchar_t *name,
                        const int has_ext, const wchar_t *pathext, wchar_t *out, const size_t out_len)
{
    const wchar_t *ext = pathext;
    const wchar_t *end;

    if (has_ext && tryCandidate(dir, dir_len, name, L"", 0, out, out_len))
        return 1;
    while (*ext)
    {
        end = wcschr(ext, L';');
        if (!end)
            end = ext + wcslen(ext);
        if (end > ext && tryCandidate(dir, dir_len, name, ext, (size_t) (end - ext), out, out_len))
            return 1;
        ext = *end ? end + 1 : end;
    }
    return 0;
}

/**
 * @brief Finds the program CreateProcess should run for `name`, the way
 * PowerShell would: a path is used as given; a bare name is looked up in
 * each PATH directory, with each PATHEXT extension (.exe, .cmd, ...).
 * Deliberately never the current directory — that's where the files
 * being edited are, not where programs should come from.
 * @param out Receives the full path.
 * @param is_batch Set when it's a .cmd/.bat script, which only runs
 * through cmd.exe.
 * @return 1 if found.
 */
static int resolveProgram(const char *name, wchar_t *out, const DWORD out_len, int *is_batch)
{
    wchar_t *wname = toWide(name);
    const wchar_t *pathext = _wgetenv(L"PATHEXT");
    const wchar_t *path = _wgetenv(L"PATH");
    const wchar_t *dir;
    const wchar_t *end;
    const wchar_t *base;
    const wchar_t *dot;
    int has_ext;
    int found = 0;

    *is_batch = 0;
    if (!wname)
        return 0;
    if (!pathext || !*pathext)
        pathext = L".COM;.EXE;.BAT;.CMD";

    base = wname + wcscspn(wname, L"");
    while (base > wname && base[-1] != L'\\' && base[-1] != L'/' && base[-1] != L':')
        base--;
    dot = wcsrchr(base, L'.');
    has_ext = dot != NULL;

    if (base != wname) // contains a directory: use it as given
        found = tryDirectory(L"", 0, wname, has_ext, pathext, out, out_len);
    else if (path)
    {
        for (dir = path; *dir && !found; dir = *end ? end + 1 : end)
        {
            end = wcschr(dir, L';');
            if (!end)
                end = dir + wcslen(dir);
            if (end > dir)
                found = tryDirectory(dir, (size_t) (end - dir), wname, has_ext, pathext, out,
                                     out_len);
        }
    }
    free(wname);

    if (found)
    {
        dot = wcsrchr(out, L'.');
        *is_batch = dot && (_wcsicmp(dot, L".cmd") == 0 || _wcsicmp(dot, L".bat") == 0);
    }
    return found;
}

static int comspec(wchar_t *out, const DWORD out_len)
{
    DWORD n = GetEnvironmentVariableW(L"ComSpec", out, out_len);
    if (n > 0 && n < out_len)
        return 1;
    n = GetSystemDirectoryW(out, out_len);
    if (n == 0 || n + 9 > out_len)
        return 0;
    wcscat(out, L"\\cmd.exe");
    return 1;
}

/**
 * @brief Builds the command line for argv, and picks the program to run.
 * @details A .cmd/.bat script can't be started directly: it runs as
 * `cmd.exe /d /s /c ""script" "arg" ..."`. cmd.exe expands %VAR% even
 * inside quotes and treats a stray quote as the end of one, so arguments
 * containing % or " (or line breaks) are refused for scripts rather than
 * risk them being executed as commands.
 */
static int buildCommandLine(char *const argv[], wchar_t *app, const DWORD app_len, WBuf *cmd)
{
    wchar_t resolved[MAX_PATH * 2];
    wchar_t *warg;
    int is_batch;
    int i;
    int ok = 1;

    if (!resolveProgram(argv[0], resolved, sizeof(resolved) / sizeof(resolved[0]), &is_batch))
    {
        errno = ENOENT;
        return 0;
    }

    if (is_batch)
    {
        for (i = 1; argv[i]; i++)
            if (strpbrk(argv[i], "%\"\r\n"))
            {
                errno = EINVAL;
                return 0;
            }
        if (!comspec(app, app_len))
        {
            errno = ENOENT;
            return 0;
        }
        ok = wbufAppend(cmd, L"cmd.exe /d /s /c \"") && appendQuotedArg(cmd, resolved, 1);
    }
    else
    {
        wcsncpy(app, resolved, app_len - 1);
        app[app_len - 1] = L'\0';
        ok = appendQuotedArg(cmd, resolved, 0);
    }

    for (i = 1; ok && argv[i]; i++)
    {
        warg = toWide(argv[i]);
        ok = warg && wbufAppend(cmd, L" ") && appendQuotedArg(cmd, warg, is_batch);
        free(warg);
    }
    if (ok && is_batch)
        ok = wbufAppend(cmd, L"\"");
    if (!ok)
        errno = ENOMEM;
    return ok;
}

/**
 * @brief A copy of this process's environment with `extra` ("NAME=value")
 * added, for CREATE_UNICODE_ENVIRONMENT. malloc'd; NULL on failure.
 */
static wchar_t *environmentWith(const wchar_t *extra)
{
    wchar_t *env = GetEnvironmentStringsW();
    const wchar_t *p;
    size_t len;
    size_t extra_len = wcslen(extra) + 1;
    wchar_t *block;

    if (!env)
        return NULL;
    for (p = env; *p; p += wcslen(p) + 1)
        ;
    len = (size_t) (p - env);
    block = malloc((len + extra_len + 1) * sizeof(wchar_t));
    if (block)
    {
        memcpy(block, env, len * sizeof(wchar_t));
        memcpy(block + len, extra, extra_len * sizeof(wchar_t));
        block[len + extra_len] = L'\0';
    }
    FreeEnvironmentStringsW(env);
    return block;
}

/***********************************************************************************************************************
 *
 *                                                 Streams and waiting
 *
 **********************************************************************************************************************/

typedef enum { STREAM_CONSOLE, STREAM_PIPE, STREAM_EMPTY } StreamKind;

struct OsStream {
    StreamKind kind;
    HANDLE h;           // the pipe's read end
    HANDLE writer;      // process whose exit ends the stream (a pane's shell:
                        // a pseudoconsole's pipe stays open after it exits)
    int eof;
};

static HANDLE consoleIn(void) { return GetStdHandle(STD_INPUT_HANDLE); }
static HANDLE consoleOut(void) { return GetStdHandle(STD_OUTPUT_HANDLE); }

static void streamInitPipe(OsStream *s, const HANDLE h, const HANDLE writer)
{
    s->kind = STREAM_PIPE;
    s->h = h;
    s->writer = writer;
    s->eof = 0;
}

static void streamInitEmpty(OsStream *s)
{
    s->kind = STREAM_EMPTY;
    s->h = NULL;
    s->writer = NULL;
    s->eof = 1;
}

static void streamClose(OsStream *s)
{
    if (s->kind == STREAM_PIPE && s->h)
        CloseHandle(s->h);
    s->h = NULL;
    s->eof = 1;
}

/**
 * @brief Bytes waiting in a pipe stream: >0 available, 0 none yet, -1 its
 * end (writer gone, or its process exited with nothing left to read).
 */
static long pipeAvailable(OsStream *s)
{
    DWORD avail = 0;
    if (s->eof)
        return -1;
    if (!PeekNamedPipe(s->h, NULL, 0, NULL, &avail, NULL))
    {
        s->eof = 1;
        return -1;
    }
    if (avail > 0)
        return (long) avail;
    if (s->writer && WaitForSingleObject(s->writer, 0) == WAIT_OBJECT_0)
    {
        // Exited: one more look, for output written just before it did.
        if (PeekNamedPipe(s->h, NULL, 0, NULL, &avail, NULL) && avail > 0)
            return (long) avail;
        s->eof = 1;
        return -1;
    }
    return 0;
}

static int consoleHasInput(void);
static long consoleRead(void *buf, size_t n, int timeout_ms);

/**
 * @brief Would os_stream_read() return without waiting? (Data, or end.)
 */
static int streamReady(OsStream *s)
{
    switch (s->kind)
    {
    case STREAM_CONSOLE: return consoleHasInput();
    case STREAM_PIPE:    return pipeAvailable(s) != 0;
    default:             return 1;
    }
}

int os_wait(OsStream *const *streams, const int n, const int timeout_ms, int *ready)
{
    const ULONGLONG start = GetTickCount64();
    ULONGLONG now;
    DWORD slice;
    int watch_console = 0;
    int has_pipes = 0;
    int count;
    int i;

    for (i = 0; i < n; i++)
    {
        if (!streams[i])
            continue;
        if (streams[i]->kind == STREAM_CONSOLE)
            watch_console = 1;
        else if (streams[i]->kind == STREAM_PIPE)
            has_pipes = 1;
    }

    for (;;)
    {
        count = 0;
        for (i = 0; i < n; i++)
        {
            ready[i] = streams[i] ? streamReady(streams[i]) : 0;
            count += ready[i];
        }
        if (count > 0 || timeout_ms == 0)
            return count;

        now = GetTickCount64();
        if (timeout_ms > 0 && now - start >= (ULONGLONG) timeout_ms)
            return 0;
        slice = timeout_ms < 0 ? INFINITE : (DWORD) ((ULONGLONG) timeout_ms - (now - start));
        // Pipes can only be peeked: look again every 10ms.
        if (has_pipes && slice > 10)
            slice = 10;
        if (watch_console)
            WaitForSingleObject(consoleIn(), slice);
        else
            Sleep(slice == INFINITE ? 10 : slice);
    }
}

long os_stream_read(OsStream *s, void *buf, const size_t n, const int timeout_ms)
{
    OsStream *one[1];
    int ready;
    long avail;
    DWORD got = 0;

    if (!s || n == 0)
        return s ? 0 : -1;
    if (s->kind == STREAM_CONSOLE)
        return consoleRead(buf, n, timeout_ms);
    if (s->kind == STREAM_EMPTY || s->eof)
        return -1;

    one[0] = s;
    if (!os_wait(one, 1, timeout_ms, &ready))
        return 0;
    avail = pipeAvailable(s);
    if (avail < 0)
        return -1;
    if (avail == 0)
        return 0;
    if (!ReadFile(s->h, buf, (DWORD) ((size_t) avail < n ? (size_t) avail : n), &got, NULL))
    {
        s->eof = 1;
        return -1;
    }
    return (long) got;
}

/***********************************************************************************************************************
 *
 *                                                 The terminal the editor runs in
 *
 **********************************************************************************************************************/

static DWORD s_orig_in_mode;
static DWORD s_orig_out_mode;
static int s_raw;
static int s_vt_input; // the console accepted ENABLE_VIRTUAL_TERMINAL_INPUT
static OsStream s_term_input = { STREAM_CONSOLE, NULL, NULL, 0 };

static void inputQueueClear(void);

int os_term_enter_raw(void)
{
    DWORD in_mode;
    DWORD out_mode;

    if (!GetConsoleMode(consoleIn(), &s_orig_in_mode) ||
        !GetConsoleMode(consoleOut(), &s_orig_out_mode))
        return -1; // not a console

    // Keys as xterm byte sequences, no echo, no line editing, and Ctrl-C
    // delivered as a key (0x03) rather than as a signal. Quick-edit (mouse
    // selection in classic conhost) is left as the user set it.
    in_mode = ENABLE_EXTENDED_FLAGS | (s_orig_in_mode & ENABLE_QUICK_EDIT_MODE) |
              ENABLE_VIRTUAL_TERMINAL_INPUT;
    s_vt_input = SetConsoleMode(consoleIn(), in_mode) != 0;
    if (!s_vt_input)
        SetConsoleMode(consoleIn(), ENABLE_EXTENDED_FLAGS | (s_orig_in_mode & ENABLE_QUICK_EDIT_MODE));

    out_mode = s_orig_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
               DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(consoleOut(), out_mode) &&
        !SetConsoleMode(consoleOut(), out_mode & ~(DWORD) DISABLE_NEWLINE_AUTO_RETURN))
    {
        // No VT support: Windows older than 10 (1809). Nothing would render.
        SetConsoleMode(consoleIn(), s_orig_in_mode);
        return -1;
    }

    FlushConsoleInputBuffer(consoleIn());
    inputQueueClear();
    s_raw = 1;
    return 0;
}

void os_term_leave_raw(void)
{
    if (!s_raw)
        return;
    SetConsoleMode(consoleIn(), s_orig_in_mode);
    SetConsoleMode(consoleOut(), s_orig_out_mode);
    s_raw = 0;
}

int os_term_size(int *rows, int *cols)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(consoleOut(), &info))
        return -1;
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return 0;
}

void os_term_write(const void *buf, const size_t n)
{
    const char *p = buf;
    size_t left = n;
    DWORD written;
    while (left > 0)
    {
        if (!WriteFile(consoleOut(), p, (DWORD) (left > 65536 ? 65536 : left), &written, NULL) ||
            written == 0)
            return;
        p += written;
        left -= written;
    }
}

OsStream *os_term_input(void)
{
    return &s_term_input;
}

/***********************************************************************************************************************
 *
 *                                                 Console: keyboard input
 *
 **********************************************************************************************************************/

// Keyboard input, as the UTF-8 bytes a Unix terminal would have sent.
static unsigned char s_inq[4096];
static size_t s_inq_head;
static size_t s_inq_len;
static wchar_t s_high_surrogate;

static void inputQueueClear(void)
{
    s_inq_head = s_inq_len = 0;
    s_high_surrogate = 0;
}

static void inputQueuePush(const unsigned char *bytes, const size_t n)
{
    size_t i;
    for (i = 0; i < n && s_inq_len < sizeof(s_inq); i++)
    {
        s_inq[(s_inq_head + s_inq_len) % sizeof(s_inq)] = bytes[i];
        s_inq_len++;
    }
}

static void inputQueuePushCodepoint(const unsigned long cp)
{
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (unsigned char) cp; n = 1; }
    else if (cp < 0x800)
    {
        b[0] = (unsigned char) (0xC0 | (cp >> 6));
        b[1] = (unsigned char) (0x80 | (cp & 0x3F));
        n = 2;
    }
    else if (cp < 0x10000)
    {
        b[0] = (unsigned char) (0xE0 | (cp >> 12));
        b[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char) (0x80 | (cp & 0x3F));
        n = 3;
    }
    else
    {
        b[0] = (unsigned char) (0xF0 | (cp >> 18));
        b[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char) (0x80 | (cp & 0x3F));
        n = 4;
    }
    inputQueuePush(b, n);
}

static void inputQueuePushChar(const wchar_t c)
{
    if (c >= 0xD800 && c <= 0xDBFF)
    {
        s_high_surrogate = c;
        return;
    }
    if (c >= 0xDC00 && c <= 0xDFFF)
    {
        if (s_high_surrogate)
            inputQueuePushCodepoint(0x10000 + (((unsigned long) s_high_surrogate - 0xD800) << 10) +
                                    ((unsigned long) c - 0xDC00));
        s_high_surrogate = 0;
        return;
    }
    s_high_surrogate = 0;
    inputQueuePushCodepoint((unsigned long) c);
}

/**
 * @brief Writes the xterm sequence for a key that produced no character
 * (arrows, Home/End, Delete, Page Up/Down) into `out`; 0 if `vk` isn't one.
 * @details In VT input mode, Windows delivers these keys as ready-made
 * escape sequences, one character event per byte, and never as bare key
 * codes — so this only comes into play on consoles that deliver the bare
 * key code instead (older conhost, Wine). Modifiers use xterm's encoding
 * ("\x1b[1;5D" is Ctrl+Left), which the key parser understands.
 */
static int vkSequence(const WORD vk, const DWORD state, char *out, const size_t out_size)
{
    char final = 0;
    int tilde = 0;
    int mod = 1;

    switch (vk)
    {
    case VK_UP:     final = 'A'; break;
    case VK_DOWN:   final = 'B'; break;
    case VK_RIGHT:  final = 'C'; break;
    case VK_LEFT:   final = 'D'; break;
    case VK_HOME:   final = 'H'; break;
    case VK_END:    final = 'F'; break;
    case VK_DELETE: tilde = 3; break;
    case VK_PRIOR:  tilde = 5; break;
    case VK_NEXT:   tilde = 6; break;
    default:        return 0;
    }
    if (state & SHIFT_PRESSED) mod += 1;
    if (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) mod += 2;
    if (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) mod += 4;

    if (tilde)
        snprintf(out, out_size, mod > 1 ? "\x1b[%d;%d~" : "\x1b[%d~", tilde, mod);
    else if (mod > 1)
        snprintf(out, out_size, "\x1b[1;%d%c", mod, final);
    else
        snprintf(out, out_size, "\x1b[%c", final);
    return 1;
}

/**
 * @brief Turns one console key event into input bytes.
 */
static void translateKeyEvent(const KEY_EVENT_RECORD *k)
{
    const wchar_t c = k->uChar.UnicodeChar;
    const DWORD ctrl = k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    const DWORD alt = k->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
    char seq[16];
    WORD repeat = k->wRepeatCount ? k->wRepeatCount : 1;

    if (!k->bKeyDown)
    {
        // Alt+numpad composition delivers its character on Alt's release.
        if (k->wVirtualKeyCode == VK_MENU && c)
            inputQueuePushChar(c);
        return;
    }

    while (repeat-- > 0)
    {
        if (c)
        {
            // Without VT input, Alt+key arrives as the bare character with
            // ALT set; a terminal sends ESC first ("ALT-x" in keybindings).
            // (AltGr reports Ctrl+Alt and types a real character: no ESC.)
            if (!s_vt_input && alt && !ctrl)
                inputQueuePush((const unsigned char *) "\x1b", 1);
            inputQueuePushChar(c);
        }
        else if (ctrl && (k->wVirtualKeyCode == VK_SPACE || k->wVirtualKeyCode == '2'))
            inputQueuePush((const unsigned char *) "", 1); // Ctrl-Space: NUL, as on Unix
        else if (vkSequence(k->wVirtualKeyCode, k->dwControlKeyState, seq, sizeof(seq)))
            inputQueuePush((const unsigned char *) seq, strlen(seq));
        // Anything else without a character (Shift, Ctrl, ...): nothing.
    }
}

/**
 * @brief Moves whatever console events are waiting into the byte queue,
 * without blocking.
 */
static void drainConsoleEvents(void)
{
    INPUT_RECORD records[64];
    DWORD available;
    DWORD got;
    DWORD i;

    while (GetNumberOfConsoleInputEvents(consoleIn(), &available) && available > 0)
    {
        if (!ReadConsoleInputW(consoleIn(), records, 64, &got) || got == 0)
            return;
        for (i = 0; i < got; i++)
            if (records[i].EventType == KEY_EVENT)
                translateKeyEvent(&records[i].Event.KeyEvent);
        // Resizes, focus and mouse events: dropped. The main loop notices
        // a resize on its own by polling the window size.
    }
}

/**
 * @brief Waits up to `ms` for keyboard bytes. @return 1 if any are queued.
 */
static int waitForInput(const DWORD ms)
{
    const ULONGLONG deadline = GetTickCount64() + ms;
    ULONGLONG now;

    for (;;)
    {
        drainConsoleEvents();
        if (s_inq_len > 0)
            return 1;
        now = GetTickCount64();
        if (now >= deadline)
            return 0;
        // Signalled for any event, keys or not: drain and look again.
        if (WaitForSingleObject(consoleIn(), (DWORD) (deadline - now)) == WAIT_TIMEOUT)
            return 0;
    }
}

static int consoleHasInput(void)
{
    drainConsoleEvents();
    return s_inq_len > 0;
}

/**
 * @brief Keyboard bytes: those already queued, or the first to arrive
 * within `timeout_ms`, or 0.
 */
static long consoleRead(void *buf, const size_t n, const int timeout_ms)
{
    size_t got = 0;
    if (s_inq_len == 0 && !waitForInput(timeout_ms < 0 ? INFINITE : (DWORD) timeout_ms))
        return 0;
    while (got < n && s_inq_len > 0)
    {
        ((unsigned char *) buf)[got++] = s_inq[s_inq_head];
        s_inq_head = (s_inq_head + 1) % sizeof(s_inq);
        s_inq_len--;
    }
    return (long) got;
}

/***********************************************************************************************************************
 *
 *                                                 Programs
 *
 **********************************************************************************************************************/

struct OsProc {
    HANDLE process;   // NULL for a program that couldn't be found
    HANDLE job;       // the program and everything it starts
    OsStream out;
    HANDLE in_write;  // its stdin, with OsSpawn.stdin_pipe
    int exited;
    int exit_code;
    int killed;
};

/**
 * @brief Puts a just-created (suspended) process in a Job object of its own
 * and lets it run.
 */
static HANDLE startInJob(const PROCESS_INFORMATION *pi)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job)
    {
        memset(&limits, 0, sizeof(limits));
        // If the editor itself goes away, its children go with it (as when
        // a Unix terminal closes); and os_proc_kill() can end a whole tree.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job, pi->hProcess))
        {
            CloseHandle(job);
            job = NULL;
        }
    }
    ResumeThread(pi->hThread);
    CloseHandle(pi->hThread);
    return job;
}

static HANDLE openNul(const int for_writing)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    return CreateFileW(L"NUL", for_writing ? GENERIC_WRITE : GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
}

static HANDLE openAppend(const char *path)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    wchar_t *w = toWide(path);
    HANDLE h = INVALID_HANDLE_VALUE;
    if (w)
        h = CreateFileW(w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    return h;
}

/**
 * @brief An anonymous pipe whose child-side end is inheritable and whose
 * own end is not — so only the intended child ever holds the other end,
 * and the pipe reaches EOF when that child exits.
 */
static int makePipe(HANDLE *read_end, HANDLE *write_end, const int child_reads)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(read_end, write_end, &sa, 64 * 1024))
        return 0;
    SetHandleInformation(child_reads ? *write_end : *read_end, HANDLE_FLAG_INHERIT, 0);
    return 1;
}

OsProc *os_proc_spawn(const OsSpawn *o)
{
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL, err_h = NULL;
    HANDLE handles[3];
    int n_handles = 0;
    SIZE_T attr_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    wchar_t app[MAX_PATH * 2];
    wchar_t *cwd = NULL;
    wchar_t *env = NULL;
    WBuf cmd = { NULL, 0, 0 };
    OsProc *p;
    BOOL created = FALSE;

    if (!o || !o->argv || !o->argv[0])
        return NULL;
    p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->exit_code = -1;

    if (!buildCommandLine((char *const *) o->argv, app, sizeof(app) / sizeof(app[0]), &cmd))
    {
        free(cmd.s);
        if (errno == ENOENT)
        {
            // Not found: behave like Unix, where the child's execvp()
            // fails — a program that exits at once with 127, no output.
            streamInitEmpty(&p->out);
            p->exited = 1;
            p->exit_code = 127;
            return p;
        }
        free(p);
        return NULL;
    }

    if (!makePipe(&out_r, &out_w, 0) || (o->stdin_pipe && !makePipe(&in_r, &in_w, 1)))
        goto fail;
    if (!o->stdin_pipe)
        in_r = openNul(0);
    switch (o->stderr_mode)
    {
    case OS_STDERR_MERGE: err_h = out_w; break;
    case OS_STDERR_FILE:  err_h = o->stderr_file ? openAppend(o->stderr_file) : openNul(1); break;
    default:              err_h = openNul(1); break;
    }
    if (in_r == INVALID_HANDLE_VALUE || err_h == INVALID_HANDLE_VALUE)
        goto fail;

    // Exactly these handles are inherited, nothing else the editor has
    // open: a stray copy of another child's pipe would keep that pipe from
    // ever reaching EOF.
    handles[n_handles++] = in_r;
    handles[n_handles++] = out_w;
    if (err_h != out_w)
        handles[n_handles++] = err_h;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = malloc(attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   (SIZE_T) n_handles * sizeof(HANDLE), NULL, NULL))
        goto fail;

    if (o->no_prompt)
        env = environmentWith(L"GIT_TERMINAL_PROMPT=0");
    if (o->cwd)
        cwd = toWide(o->cwd);

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_h;
    si.lpAttributeList = attrs;

    // CREATE_NO_WINDOW: a console of its own, never shown — neither the
    // program nor anything it starts can draw on, or read keys from, the
    // console the editor runs in.
    created = CreateProcessW(app, cmd.s, NULL, NULL, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT,
                             env, cwd, &si.StartupInfo, &pi);
    if (!created)
        goto fail;

    p->process = pi.hProcess;
    p->job = startInJob(&pi);
    streamInitPipe(&p->out, out_r, NULL);
    p->in_write = in_w;
    out_r = in_w = NULL; // now p's

fail:
    if (attrs)
    {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    free(env);
    free(cwd);
    free(cmd.s);
    // The child's ends: it has its own copies now (or failed to start).
    if (err_h && err_h != out_w && err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
    if (out_w) CloseHandle(out_w);
    if (in_r && in_r != INVALID_HANDLE_VALUE) CloseHandle(in_r);
    if (!created)
    {
        if (out_r) CloseHandle(out_r);
        if (in_w) CloseHandle(in_w);
        free(p);
        return NULL;
    }
    return p;
}

OsStream *os_proc_output(OsProc *p)
{
    return p ? &p->out : NULL;
}

long os_proc_write(OsProc *p, const void *buf, const size_t n)
{
    const char *c = buf;
    size_t left = n;
    DWORD written;
    if (!p || !p->in_write)
        return -1;
    while (left > 0)
    {
        if (!WriteFile(p->in_write, c, (DWORD) (left > 65536 ? 65536 : left), &written, NULL) ||
            written == 0)
            return -1;
        c += written;
        left -= written;
    }
    return (long) n;
}

void os_proc_kill(OsProc *p)
{
    if (!p || !p->process || p->exited)
        return;
    p->killed = 1;
    if (!(p->job && TerminateJobObject(p->job, 1)))
        TerminateProcess(p->process, 1);
}

int os_proc_wait(OsProc *p, const int timeout_ms, int *exit_code)
{
    DWORD code = 0;
    if (!p)
        return 1;
    if (!p->exited)
    {
        if (WaitForSingleObject(p->process, timeout_ms < 0 ? INFINITE : (DWORD) timeout_ms) != WAIT_OBJECT_0)
            return 0;
        GetExitCodeProcess(p->process, &code);
        p->exited = 1;
        p->exit_code = p->killed ? -1 : (int) code;
    }
    if (exit_code)
        *exit_code = p->exit_code;
    return 1;
}

void os_proc_free(OsProc *p)
{
    if (!p)
        return;
    streamClose(&p->out);
    if (p->in_write)
        CloseHandle(p->in_write);
    if (p->process)
    {
        if (!p->exited)
        {
            os_proc_kill(p);
            WaitForSingleObject(p->process, INFINITE);
        }
        CloseHandle(p->process);
    }
    if (p->job)
        CloseHandle(p->job); // KILL_ON_JOB_CLOSE takes any stragglers with it
    free(p);
}

static const char *powerShell(void)
{
    static const char *found = NULL;
    wchar_t path[MAX_PATH * 2];
    int is_batch;
    if (!found)
        found = resolveProgram("pwsh", path, MAX_PATH * 2, &is_batch) ? "pwsh" : "powershell";
    return found;
}

static char *powerShellEncode(const char *script);

static char *dupString(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char **os_shell_argv(const char *command)
{
    // UTF-8 output whatever the console code page; and the command's own
    // success as the exit code: a failing program's code, 1 for a failed
    // cmdlet, else 0.
    static const char prologue[] =
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "$OutputEncoding = [Text.Encoding]::UTF8\n";
    static const char epilogue[] =
        "\n$__beluga_ok = $?; if ($LASTEXITCODE) { exit $LASTEXITCODE }; "
        "if (-not $__beluga_ok) { exit 1 }";
    char **argv = calloc(6, sizeof(char *));
    size_t len = sizeof(prologue) + strlen(command) + sizeof(epilogue);
    char *script = malloc(len);

    if (!argv || !script)
    {
        free(argv);
        free(script);
        return NULL;
    }
    snprintf(script, len, "%s%s%s", prologue, command, epilogue);
    // -EncodedCommand: base64 of the UTF-16 script, which no layer of
    // command-line quoting can alter.
    argv[0] = dupString(powerShell());
    argv[1] = dupString("-NoProfile");
    argv[2] = dupString("-NonInteractive");
    argv[3] = dupString("-EncodedCommand");
    argv[4] = powerShellEncode(script);
    free(script);
    if (!argv[0] || !argv[1] || !argv[2] || !argv[3] || !argv[4])
    {
        os_free_argv(argv);
        return NULL;
    }
    return argv;
}

void os_free_argv(char **argv)
{
    char **a;
    if (!argv)
        return;
    for (a = argv; *a; a++)
        free(*a);
    free(argv);
}

char *os_shell_quote(const char *s)
{
    // PowerShell single quotes: everything literal, ' doubled — and so are
    // the typographic ones (U+2018-U+201B), which PowerShell also treats
    // as quote characters.
    const unsigned char *p;
    char *out = malloc(strlen(s) * 2 + 3);
    size_t o = 0;
    if (!out)
        return NULL;
    out[o++] = '\'';
    for (p = (const unsigned char *) s; *p; p++)
    {
        if (*p == '\'')
            out[o++] = '\'';
        else if (p[0] == 0xE2 && p[1] == 0x80 && p[2] >= 0x98 && p[2] <= 0x9B)
        {
            memcpy(out + o, p, 3);
            o += 3;
        }
        out[o++] = (char) *p;
    }
    out[o++] = '\'';
    out[o] = '\0';
    return out;
}

/***********************************************************************************************************************
 *
 *                                                 Terminal panes (ConPTY)
 *
 **********************************************************************************************************************/

struct OsPty {
    HPCON console;
    HANDLE input;     // keys for the shell
    HANDLE process;
    HANDLE job;
    OsStream out;     // the pseudoconsole's output; ends when the shell exits
};

/**
 * @brief The shell a terminal pane runs when none is given: $BELUGA_SHELL,
 * then PowerShell 7 (pwsh), then Windows PowerShell, then cmd.exe.
 */
static void defaultShellCommand(WBuf *cmd)
{
    wchar_t path[MAX_PATH * 2];
    const wchar_t *configured = _wgetenv(L"BELUGA_SHELL");
    int is_batch;

    if (configured && *configured)
    {
        wbufAppend(cmd, configured);
        return;
    }
    if (resolveProgram("pwsh", path, MAX_PATH * 2, &is_batch) ||
        resolveProgram("powershell", path, MAX_PATH * 2, &is_batch))
    {
        appendQuotedArg(cmd, path, 0);
        wbufAppend(cmd, L" -NoLogo");
        return;
    }
    if (comspec(path, MAX_PATH * 2))
        appendQuotedArg(cmd, path, 0);
}

static DWORD WINAPI closePseudoConsoleThread(LPVOID console)
{
    ClosePseudoConsole((HPCON) console);
    return 0;
}

OsPty *os_pty_open(const char *shell, const int rows, const int cols)
{
    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    HPCON console = NULL;
    COORD size;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attr_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    WBuf cmd = { NULL, 0, 0 };
    wchar_t *wshell;
    wchar_t *env = NULL;
    OsPty *p = calloc(1, sizeof(*p));

    if (!p)
        return NULL;
    if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0))
        goto fail;

    size.X = (SHORT) (cols > 0 ? cols : 80);
    size.Y = (SHORT) (rows > 0 ? rows : 24);
    if (FAILED(CreatePseudoConsole(size, in_read, out_write, 0, &console)))
        goto fail;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = malloc(attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                                   sizeof(console), NULL, NULL))
        goto fail;

    if (shell && *shell)
    {
        wshell = toWide(shell);
        if (wshell)
            appendQuotedArg(&cmd, wshell, 0);
        free(wshell);
    }
    else
        defaultShellCommand(&cmd);
    if (!cmd.s)
        goto fail;

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    // STARTF_USESTDHANDLES with no handles at all: when the editor's own
    // stdio isn't a console (redirected, a debugger), CreateProcess would
    // otherwise hand those to the shell, which then writes there instead
    // of to the pseudoconsole. With null handles, the console fills them
    // in from the pseudoconsole as the shell attaches — what Windows
    // Terminal and VS Code's node-pty do too.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;

    env = environmentWith(L"TERM=xterm-256color"); // as a Unix pane gets
    if (!CreateProcessW(NULL, cmd.s, NULL, NULL, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        env, NULL, &si.StartupInfo, &pi))
        goto fail;

    p->process = pi.hProcess;
    p->job = startInJob(&pi);
    p->console = console;
    p->input = in_write;
    streamInitPipe(&p->out, out_read, pi.hProcess);
    // The pseudoconsole holds its own copies of these two.
    CloseHandle(in_read);
    CloseHandle(out_write);
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    free(env);
    free(cmd.s);
    return p;

fail:
    if (attrs)
    {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    free(env);
    free(cmd.s);
    if (console) ClosePseudoConsole(console);
    if (in_read) CloseHandle(in_read);
    if (in_write) CloseHandle(in_write);
    if (out_read) CloseHandle(out_read);
    if (out_write) CloseHandle(out_write);
    free(p);
    return NULL;
}

OsStream *os_pty_output(OsPty *p)
{
    return p ? &p->out : NULL;
}

long os_pty_write(OsPty *p, const void *buf, const size_t n)
{
    DWORD written = 0;
    if (!p || !p->input || !WriteFile(p->input, buf, (DWORD) n, &written, NULL))
        return -1;
    return (long) written;
}

void os_pty_resize(OsPty *p, const int rows, const int cols)
{
    COORD size;
    if (!p)
        return;
    size.X = (SHORT) cols;
    size.Y = (SHORT) rows;
    ResizePseudoConsole(p->console, size);
}

int os_pty_alive(OsPty *p)
{
    return p && WaitForSingleObject(p->process, 0) == WAIT_TIMEOUT;
}

void os_pty_close(OsPty *p)
{
    HANDLE thread;
    if (!p)
        return;
    if (p->job)
        TerminateJobObject(p->job, 1);
    else
        TerminateProcess(p->process, 1);
    WaitForSingleObject(p->process, 2000);
    CloseHandle(p->input);
    streamClose(&p->out);
    // ClosePseudoConsole() can wait for its last output to be read, which
    // nothing will do now: let it finish on a thread of its own.
    thread = CreateThread(NULL, 0, closePseudoConsoleThread, p->console, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        ClosePseudoConsole(p->console);
    CloseHandle(p->process);
    if (p->job)
        CloseHandle(p->job);
    free(p);
}

/***********************************************************************************************************************
 *
 *                                                 Files and directories
 *
 **********************************************************************************************************************/

/**
 * @brief FILETIME (100ns since 1601) to seconds since the Unix epoch.
 */
static long long unixTime(const FILETIME *ft)
{
    ULARGE_INTEGER t;
    t.LowPart = ft->dwLowDateTime;
    t.HighPart = ft->dwHighDateTime;
    return (long long) (t.QuadPart / 10000000ULL) - 11644473600LL;
}

OsDirEntry *os_list_dir(const char *path, int *count)
{
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t *pattern;
    char *spec;
    OsDirEntry *list;
    OsDirEntry *grown;
    size_t len = strlen(path);
    int cap = 64;
    int n = 0;

    *count = 0;
    spec = malloc(len + 3);
    if (!spec)
        return NULL;
    snprintf(spec, len + 3, "%s%s*", path, (len && (path[len - 1] == '/' || path[len - 1] == '\\')) ? "" : "/");
    pattern = toWide(spec);
    free(spec);
    if (!pattern)
        return NULL;
    find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE)
        return NULL;
    list = malloc((size_t) cap * sizeof(*list));
    if (!list)
    {
        FindClose(find);
        return NULL;
    }
    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (n == cap)
        {
            cap *= 2;
            grown = realloc(list, (size_t) cap * sizeof(*list));
            if (!grown)
                break;
            list = grown;
        }
        list[n].name = toUtf8(fd.cFileName);
        if (!list[n].name)
            break;
        list[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        n++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    *count = n;
    return list;
}

void os_free_dir(OsDirEntry *entries, const int count)
{
    int i;
    if (!entries)
        return;
    for (i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
}

int os_file_info(const char *path, OsFileInfo *info)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    wchar_t *w = toWide(path);
    char *target;
    ULARGE_INTEGER size;
    int ok;

    memset(info, 0, sizeof(*info));
    ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &data);
    free(w);
    if (!ok)
        return -1;

    info->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    // Symbolic links and junctions are reparse points.
    info->is_link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    info->size = (long long) size.QuadPart;
    info->mtime = unixTime(&data.ftLastWriteTime);
    // No Unix permission bits on Windows: read-only is what there is.
    snprintf(info->mode, sizeof(info->mode), "%c%s", info->is_link ? 'l' : info->is_dir ? 'd' : '-',
             (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? "r--r--r--" : "rw-rw-rw-");
    if (info->is_link && (target = os_realpath(path)) != NULL)
    {
        snprintf(info->link_target, sizeof(info->link_target), "%s", target);
        free(target);
    }
    return 0;
}

int os_file_flush(FILE *f)
{
    if (fflush(f) != 0)
        return -1;
    return _commit(_fileno(f)) == 0 ? 0 : -1;
}

int os_replace_file(const char *tmp, const char *dest, char *err, size_t err_size)
{
    wchar_t *wtmp = toWide(tmp);
    wchar_t *wdest = toWide(dest);
    DWORD attrs;
    int ok;
    if (!wtmp || !wdest)
    {
        free(wtmp);
        free(wdest);
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    // Keep the original's attributes (hidden, ...). A read-only file isn't
    // replaced: MoveFileEx refuses it, as on Unix without write permission.
    attrs = GetFileAttributesW(wdest);
    if (attrs != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(wtmp, attrs & ~FILE_ATTRIBUTE_READONLY);
    ok = MoveFileExW(wtmp, wdest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok)
        snprintf(err, err_size, "can't replace the file (error %lu)", (unsigned long) GetLastError());
    free(wtmp);
    free(wdest);
    return ok ? 0 : -1;
}

/**
 * @brief The absolute, canonical path of an existing file, with forward
 * slashes. GetFinalPathNameByHandle resolves links and corrects the case,
 * so "Main.C" and "main.c" open as the same buffer, being the same file.
 */
char *os_realpath(const char *path)
{
    wchar_t *wpath = toWide(path);
    wchar_t final[OS_PATH_MAX];
    const wchar_t *p = final;
    HANDLE h;
    DWORD n;
    char *utf8;

    if (!wpath)
        return NULL;
    h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    n = GetFinalPathNameByHandleW(h, final, OS_PATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= OS_PATH_MAX)
        return NULL;

    // \\?\C:\dir -> C:\dir ; \\?\UNC\server\share -> \\server\share
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0)
    {
        p += 6;
        final[6] = L'\\';
    }
    else if (wcsncmp(p, L"\\\\?\\", 4) == 0)
        p += 4;

    utf8 = toUtf8(p);
    backslashesToSlashes(utf8);
    return utf8;
}

char *os_getcwd(void)
{
    wchar_t buf[OS_PATH_MAX];
    char *utf8;
    if (!GetCurrentDirectoryW(OS_PATH_MAX, buf))
        return NULL;
    utf8 = toUtf8(buf);
    backslashesToSlashes(utf8);
    return utf8;
}

const char *os_home_dir(void)
{
    static char *home = NULL;
    const wchar_t *profile;
    if (!home)
    {
        profile = _wgetenv(L"USERPROFILE");
        home = profile ? toUtf8(profile) : NULL;
        if (!home)
            home = dupString("C:/");
        backslashesToSlashes(home);
    }
    return home;
}

int os_path_drive_length(const char *path)
{
    const char c = path ? (char) (path[0] | 0x20) : 0;
    return (c >= 'a' && c <= 'z' && path[1] == ':') ? 2 : 0;
}

int os_path_is_absolute(const char *path)
{
    if (!path)
        return 0;
    if (path[0] == '/' || path[0] == '\\') // "//server/share", or the drive's root
        return 1;
    return os_path_drive_length(path) == 2 && (path[2] == '/' || path[2] == '\\');
}

void os_path_to_slashes(char *path)
{
    backslashesToSlashes(path);
}

int os_path_equal(const char *a, const char *b)
{
    wchar_t *wa = toWide(a);
    wchar_t *wb = toWide(b);
    int equal = wa && wb && CompareStringOrdinal(wa, -1, wb, -1, TRUE) == CSTR_EQUAL;
    free(wa);
    free(wb);
    return equal;
}

// ─── File operations (dired) ────────────────────────────────────────────────

#define FILEOP_DELETE 1
#define FILEOP_MOVE 2
#define FILEOP_COPY 3

/**
 * @brief Win32 error code to a one-line UTF-8 message.
 */
static void win32ErrorMessage(const DWORD code, char *err, const size_t err_size)
{
    wchar_t wmsg[256];
    char *msg;
    size_t len;

    if (!err || err_size == 0)
        return;
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                       wmsg, sizeof(wmsg) / sizeof(wmsg[0]), NULL) &&
        (msg = toUtf8(wmsg)) != NULL)
    {
        len = strlen(msg);
        while (len > 0 && (msg[len - 1] == '\r' || msg[len - 1] == '\n' || msg[len - 1] == ' ' ||
                           msg[len - 1] == '.'))
            msg[--len] = '\0';
        snprintf(err, err_size, "%s", msg);
        free(msg);
    }
    else
        snprintf(err, err_size, "error %lu", (unsigned long) code);
}

/**
 * @brief `paths` as SHFileOperation wants them: absolute, backslashes,
 * each NUL-terminated, the whole list ending in an extra NUL. malloc'd.
 */
static wchar_t *doubleNulList(const char *const *paths, const int count)
{
    WBuf list = { NULL, 0, 0 };
    wchar_t full[OS_PATH_MAX];
    wchar_t *w;
    wchar_t *p;
    const wchar_t nul = L'\0';
    int i;

    for (i = 0; i < count; i++)
    {
        w = toWide(paths[i]);
        if (!w || !GetFullPathNameW(w, OS_PATH_MAX, full, NULL))
        {
            free(w);
            free(list.s);
            return NULL;
        }
        free(w);
        for (p = full; *p; p++)
            if (*p == L'/')
                *p = L'\\';
        if (!wbufAppend(&list, full) || !wbufAppendN(&list, &nul, 1))
        {
            free(list.s);
            return NULL;
        }
    }
    if (!wbufAppendN(&list, &nul, 1)) // the list's terminator (wbuf adds one more)
    {
        free(list.s);
        return NULL;
    }
    return list.s;
}

static int fileOperation(const int op, const char *const *paths, const int count,
                        const char *dest, char *err, const size_t err_size)
{
    SHFILEOPSTRUCTW fo;
    wchar_t *from;
    wchar_t *to = NULL;
    int result;

    if (err && err_size)
        err[0] = '\0';
    if (count <= 0)
        return 0;
    from = doubleNulList(paths, count);
    if (op != FILEOP_DELETE)
        to = dest ? doubleNulList(&dest, 1) : NULL;
    if (!from || (op != FILEOP_DELETE && !to))
    {
        free(from);
        free(to);
        snprintf(err, err_size, "invalid path");
        return ERROR_INVALID_NAME;
    }

    memset(&fo, 0, sizeof(fo));
    fo.wFunc = op == FILEOP_DELETE ? FO_DELETE : op == FILEOP_MOVE ? FO_MOVE : FO_COPY;
    fo.pFrom = from;
    fo.pTo = to;
    // No dialogs or progress windows, no questions (overwrite, like mv/cp),
    // and no FOF_ALLOWUNDO: a delete is permanent, as rm -rf is.
    fo.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT;

    result = SHFileOperationW(&fo);
    if (result == 0 && fo.fAnyOperationsAborted)
        result = ERROR_CANCELLED;
    if (result != 0)
    {
        // SHFileOperation returns either a Win32 error or one of its own
        // legacy DE_* codes (below 0x80, or 0x10000 and up) that
        // FormatMessage doesn't know.
        if (result >= 0x71 && result <= 0x88)
            snprintf(err, err_size, "failed (code 0x%X) — a path may be invalid or in use", result);
        else
            win32ErrorMessage((DWORD) result, err, err_size);
    }
    free(from);
    free(to);
    return result;
}

static int makeDirectories(const char *path, char *err, const size_t err_size)
{
    wchar_t *w = toWide(path);
    wchar_t full[OS_PATH_MAX];
    wchar_t *p;
    int result;

    if (err && err_size)
        err[0] = '\0';
    if (!w || !GetFullPathNameW(w, OS_PATH_MAX, full, NULL))
    {
        free(w);
        snprintf(err, err_size, "invalid path");
        return ERROR_INVALID_NAME;
    }
    free(w);
    for (p = full; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    result = SHCreateDirectoryExW(NULL, full, NULL);
    if (result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS)
    {
        // mkdir -p: fine if it's already a directory, an error if a file.
        result = (GetFileAttributesW(full) & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ERROR_FILE_EXISTS;
    }
    if (result != 0)
        win32ErrorMessage((DWORD) result, err, err_size);
    return result;
}

/**
 * @brief Would moving or copying `src` to `dest` put a directory inside
 * itself (cp -R a a/b)? Refused up front, as on Unix: the shell's file
 * operation doesn't reliably refuse it.
 */
static int insideItself(const char *src, const char *dest)
{
    OsFileInfo info;
    char parent[OS_PATH_MAX];
    char *rs = os_realpath(src);
    char *rp;
    char *slash;
    size_t len;
    int inside = 0;

    if (!rs)
        return 0;
    snprintf(parent, sizeof(parent), "%s", dest);
    os_path_to_slashes(parent);
    if (os_file_info(parent, &info) != 0 || !info.is_dir)
    {
        slash = strrchr(parent, '/'); // dest itself doesn't exist: its parent
        if (slash)
            *slash = '\0';
    }
    rp = os_realpath(parent);
    if (rp)
    {
        len = strlen(rs);
        if (strlen(rp) >= len && (rp[len] == '/' || rp[len] == '\0'))
        {
            rp[len] = '\0';
            inside = os_path_equal(rp, rs);
        }
        free(rp);
    }
    free(rs);
    return inside;
}

static int transfer(const int op, const char *const *sources, const int count, const char *dest,
                    char *err, const size_t err_size)
{
    int i;
    for (i = 0; i < count; i++)
        if (insideItself(sources[i], dest))
        {
            snprintf(err, err_size, "can't put %s inside itself", sources[i]);
            return -1;
        }
    return fileOperation(op, sources, count, dest, err, err_size) == 0 ? 0 : -1;
}

int os_delete_tree(const char *path, char *err, const size_t err_size)
{
    return fileOperation(FILEOP_DELETE, &path, 1, NULL, err, err_size) == 0 ? 0 : -1;
}

int os_move(const char *const *sources, const int count, const char *dest, char *err,
            const size_t err_size)
{
    return transfer(FILEOP_MOVE, sources, count, dest, err, err_size);
}

int os_copy_tree(const char *const *sources, const int count, const char *dest, char *err,
                 const size_t err_size)
{
    return transfer(FILEOP_COPY, sources, count, dest, err, err_size);
}

int os_mkdirs(const char *path, char *err, const size_t err_size)
{
    return makeDirectories(path, err, err_size) == 0 ? 0 : -1;
}

/***********************************************************************************************************************
 *
 *                                                 Clipboard
 *
 **********************************************************************************************************************/

/**
 * @brief A hidden message-only window to own the clipboard: with no owner
 * window, SetClipboardData() is documented to fail after EmptyClipboard().
 */
static HWND clipboardWindow(void)
{
    static HWND hwnd = NULL;
    if (!hwnd)
        hwnd = CreateWindowExW(0, L"STATIC", L"beluga-clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE,
                               NULL, GetModuleHandleW(NULL), NULL);
    return hwnd;
}

static int openClipboard(void)
{
    int attempt;
    // Another program may hold it for a moment (clipboard managers do).
    for (attempt = 0; attempt < 10; attempt++)
    {
        if (OpenClipboard(clipboardWindow()))
            return 1;
        Sleep(10);
    }
    return 0;
}

char *os_clipboard_get(void)
{
    HANDLE data;
    const wchar_t *wtext;
    char *text = NULL;
    char *src;
    char *dst;

    if (!openClipboard())
        return NULL;
    data = GetClipboardData(CF_UNICODETEXT);
    if (data && (wtext = GlobalLock(data)) != NULL)
    {
        text = toUtf8(wtext);
        GlobalUnlock(data);
    }
    CloseClipboard();

    if (text) // CRLF -> LF
    {
        for (src = dst = text; *src; src++)
            if (!(src[0] == '\r' && src[1] == '\n'))
                *dst++ = *src;
        *dst = '\0';
    }
    return text;
}

static int clipboardSet(const char *text, const size_t len)
{
    char *crlf;
    wchar_t *wide;
    HGLOBAL mem;
    void *dst;
    size_t i;
    size_t j = 0;
    int wlen;
    int ok = 0;

    crlf = malloc(len * 2 + 1); // LF -> CRLF, what Windows apps expect
    if (!crlf)
        return 0;
    for (i = 0; i < len; i++)
    {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
            crlf[j++] = '\r';
        crlf[j++] = text[i];
    }
    crlf[j] = '\0';
    wide = toWide(crlf);
    free(crlf);
    if (!wide)
        return 0;

    wlen = (int) wcslen(wide) + 1;
    mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T) wlen * sizeof(wchar_t));
    if (mem && (dst = GlobalLock(mem)) != NULL)
    {
        memcpy(dst, wide, (size_t) wlen * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (openClipboard())
        {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
            CloseClipboard();
        }
    }
    if (!ok && mem)
        GlobalFree(mem); // on success the clipboard owns it
    free(wide);
    return ok;
}

int os_clipboard_set(const char *text, const size_t len)
{
    return clipboardSet(text, len) ? 0 : -1;
}

/***********************************************************************************************************************
 *
 *                                                 PowerShell
 *
 **********************************************************************************************************************/

static char *powerShellEncode(const char *script)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    wchar_t *w = toWide(script);
    const unsigned char *bytes;
    size_t n;
    size_t i;
    size_t o = 0;
    unsigned long triple;
    char *out;

    if (!w)
        return NULL;
    bytes = (const unsigned char *) w; // UTF-16LE: wchar_t is little-endian here
    n = wcslen(w) * sizeof(wchar_t);
    out = malloc((n + 2) / 3 * 4 + 1);
    if (out)
    {
        for (i = 0; i < n; i += 3)
        {
            triple = (unsigned long) bytes[i] << 16;
            if (i + 1 < n) triple |= (unsigned long) bytes[i + 1] << 8;
            if (i + 2 < n) triple |= bytes[i + 2];
            out[o++] = alphabet[(triple >> 18) & 0x3F];
            out[o++] = alphabet[(triple >> 12) & 0x3F];
            out[o++] = i + 1 < n ? alphabet[(triple >> 6) & 0x3F] : '=';
            out[o++] = i + 2 < n ? alphabet[triple & 0x3F] : '=';
        }
        out[o] = '\0';
    }
    free(w);
    return out;
}
