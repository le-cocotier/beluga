/**
 * @file platform_win32.c
 * @brief The Windows implementation of what include/platform_win32.h
 * declares: console I/O, select()/poll(), processes, ConPTY, the
 * clipboard, and the few C library functions MinGW lacks.
 * @details Compiled only on Windows (see meson.build); the design is
 * described at the top of platform_win32.h. CRT functions are called by
 * their underscore names (_read, _close, ...) throughout, since the plain
 * names are redirected to the functions defined here.
 */

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wchar.h>

#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>

// This file implements the redirected functions on top of the CRT's own,
// so it needs these two by their real names.
#undef signal
#undef strftime

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
 *                                                 Start-up
 *
 **********************************************************************************************************************/

static UINT s_orig_output_cp;

static void restoreConsoleCodePage(void)
{
    if (s_orig_output_cp)
        SetConsoleOutputCP(s_orig_output_cp);
}

/**
 * @brief Runs before main(): binary file I/O, a UTF-8 console, and $HOME.
 * @details A constructor rather than a call in main() so the one shared
 * entry point stays the same on every platform.
 * - _fmode: every fopen()/open() in binary mode, so a "w" save writes the
 *   editor's own line endings (file_io.c decides CRLF vs LF per buffer)
 *   instead of the CRT turning each "\n" into "\r\n".
 * - Console output code page UTF-8: the UI is written as UTF-8 bytes.
 * - $HOME: the editor keeps its files in $HOME/.beluga, like on Unix.
 *   Windows normally has no HOME, so it's set from %USERPROFILE%, with
 *   forward slashes like every other path the editor handles.
 */
__attribute__((constructor)) static void belugaWin32Init(void)
{
    const char *profile;
    char *home;

    _fmode = _O_BINARY;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    s_orig_output_cp = GetConsoleOutputCP();
    if (s_orig_output_cp && s_orig_output_cp != CP_UTF8)
    {
        SetConsoleOutputCP(CP_UTF8);
        atexit(restoreConsoleCodePage);
    }

    if (!getenv("HOME"))
    {
        profile = getenv("USERPROFILE");
        if (profile && *profile)
        {
            home = _strdup(profile);
            if (home)
            {
                backslashesToSlashes(home);
                _putenv_s("HOME", home);
                free(home);
            }
        }
    }
}

/***********************************************************************************************************************
 *
 *                                                 Descriptor bookkeeping
 *
 **********************************************************************************************************************/

#define MAX_TRACKED_FDS 256

typedef struct {
    int nonblock;        // fcntl(F_SETFL, O_NONBLOCK)
    int is_pty;          // a terminal pane's descriptor (reads ConPTY output)
    HANDLE pty_input;    // ...and where write() on it goes
    HPCON pty_console;
    pid_t pty_pid;       // the shell: EOF is reported once it has exited
} FdInfo;

static FdInfo s_fds[MAX_TRACKED_FDS];

static FdInfo *fdInfo(const int fd)
{
    return (fd >= 0 && fd < MAX_TRACKED_FDS) ? &s_fds[fd] : NULL;
}

/***********************************************************************************************************************
 *
 *                                                 Processes
 *
 **********************************************************************************************************************/

#define MAX_PROCESSES 64

typedef struct {
    pid_t pid;      // 0: free slot
    HANDLE process;
    HANDLE job;     // the child and everything it starts
    int killed;     // ended by kill(): reported as signalled, not exited
} Process;

static Process s_procs[MAX_PROCESSES];

static Process *findProcess(const pid_t pid)
{
    int i;
    for (i = 0; i < MAX_PROCESSES; i++)
        if (s_procs[i].pid == pid && pid > 0)
            return &s_procs[i];
    return NULL;
}

static void forgetProcess(Process *p)
{
    CloseHandle(p->process);
    if (p->job)
        CloseHandle(p->job); // KILL_ON_JOB_CLOSE: takes any stragglers with it
    memset(p, 0, sizeof(*p));
}

/**
 * @brief Has `pid` (started by us) exited? Doesn't reap it.
 */
static int processExited(const pid_t pid)
{
    Process *p = findProcess(pid);
    if (!p)
        return 1;
    return WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0;
}

pid_t waitpid(const pid_t pid, int *status, const int options)
{
    Process *p = findProcess(pid);
    DWORD code = 0;
    DWORD wait;

    if (!p)
    {
        errno = ECHILD;
        return -1;
    }
    wait = WaitForSingleObject(p->process, (options & WNOHANG) ? 0 : INFINITE);
    if (wait == WAIT_TIMEOUT)
        return 0;

    GetExitCodeProcess(p->process, &code);
    if (status)
        *status = p->killed ? SIGKILL : (int) ((code & 0xff) << 8);
    forgetProcess(p);
    return pid;
}

int kill(const pid_t pid, const int sig)
{
    Process *p = findProcess(pid < 0 ? -pid : pid);

    if (!p)
    {
        errno = ESRCH;
        return -1;
    }
    if (sig == 0 || sig == SIGWINCH) // existence check / ConPTY resizes itself
        return 0;

    // Whole tree, like a process group: the job holds the child and
    // whatever it started (cmd.exe and the command it runs, git and ssh).
    p->killed = 1;
    if (p->job && TerminateJobObject(p->job, 1))
        return 0;
    if (TerminateProcess(p->process, 1))
        return 0;
    errno = EPERM;
    return -1;
}

/**
 * @brief Records a just-created (still suspended) process, puts it in a
 * Job object of its own, and lets it run.
 */
static pid_t adoptProcess(const PROCESS_INFORMATION *pi)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    Process *slot = NULL;
    HANDLE job;
    int i;

    for (i = 0; i < MAX_PROCESSES; i++)
        if (s_procs[i].pid == 0)
        {
            slot = &s_procs[i];
            break;
        }
    if (!slot)
    {
        TerminateProcess(pi->hProcess, 1);
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        errno = EAGAIN;
        return -1;
    }

    job = CreateJobObjectW(NULL, NULL);
    if (job)
    {
        memset(&limits, 0, sizeof(limits));
        // If the editor itself dies, its children go with it (as when a
        // Unix terminal closes), and kill() can end a whole tree.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
        if (!AssignProcessToJobObject(job, pi->hProcess))
        {
            CloseHandle(job);
            job = NULL;
        }
    }

    slot->pid = (pid_t) pi->dwProcessId;
    slot->process = pi->hProcess;
    slot->job = job;
    slot->killed = 0;

    ResumeThread(pi->hThread);
    CloseHandle(pi->hThread);
    return slot->pid;
}

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

// ─── Standard handles and environment ───────────────────────────────────────

/**
 * @brief An inheritable copy of fd's handle, or of the NUL device for -1.
 */
static HANDLE inheritableHandle(const int fd, const int for_writing)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE src;
    HANDLE dup = INVALID_HANDLE_VALUE;

    if (fd < 0)
        return CreateFileW(L"NUL", for_writing ? GENERIC_WRITE : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    src = (HANDLE) _get_osfhandle(fd);
    if (src == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &dup, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        return INVALID_HANDLE_VALUE;
    return dup;
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

/**
 * @brief CreateProcess with exactly three redirected standard handles.
 * @details The handle list attribute makes the child inherit only these,
 * not every inheritable handle the editor happens to have open — an
 * inherited copy of another child's pipe would keep that pipe from ever
 * reaching EOF.
 */
static pid_t spawnWithHandles(const wchar_t *app, wchar_t *cmdline, const int stdin_fd,
                              const int stdout_fd, const int stderr_fd, const unsigned flags)
{
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    HANDLE handles[3];
    HANDLE unique[3];
    int n_unique = 0;
    SIZE_T attr_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    wchar_t *env = NULL;
    BOOL created = FALSE;
    pid_t pid = -1;
    int i;
    int j;
    int seen;

    handles[0] = inheritableHandle(stdin_fd, 0);
    handles[1] = inheritableHandle(stdout_fd, 1);
    // stderr to the same descriptor as stdout: share the one handle.
    handles[2] = (stderr_fd >= 0 && stderr_fd == stdout_fd) ? handles[1]
                                                           : inheritableHandle(stderr_fd, 1);
    for (i = 0; i < 3; i++)
    {
        if (handles[i] == INVALID_HANDLE_VALUE)
        {
            errno = EBADF;
            goto out;
        }
        seen = 0;
        for (j = 0; j < n_unique; j++)
            seen |= unique[j] == handles[i];
        if (!seen)
            unique[n_unique++] = handles[i];
    }

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = malloc(attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, unique,
                                   (SIZE_T) n_unique * sizeof(HANDLE), NULL, NULL))
    {
        errno = ENOMEM;
        goto out;
    }

    if (flags & BELUGA_SPAWN_NO_PROMPT)
        env = environmentWith(L"GIT_TERMINAL_PROMPT=0");

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = handles[0];
    si.StartupInfo.hStdOutput = handles[1];
    si.StartupInfo.hStdError = handles[2];
    si.lpAttributeList = attrs;

    // CREATE_NO_WINDOW: a console of its own, never shown — neither the
    // child nor anything it starts can draw on (or read keys from) the
    // console the editor runs in.
    created = CreateProcessW(app, cmdline, NULL, NULL, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT,
                             env, NULL, &si.StartupInfo, &pi);
    if (!created)
        errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
    else
        pid = adoptProcess(&pi);

out:
    if (attrs)
    {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    free(env);
    for (i = 0; i < 3; i++)
        if (handles[i] != INVALID_HANDLE_VALUE && (i < 2 || handles[2] != handles[1]))
            CloseHandle(handles[i]);
    return pid;
}

pid_t belugaSpawn(char *const argv[], const int stdin_fd, const int stdout_fd,
                  const int stderr_fd, const unsigned flags)
{
    wchar_t app[MAX_PATH * 2];
    WBuf cmd = { NULL, 0, 0 };
    pid_t pid;

    if (!argv || !argv[0])
    {
        errno = EINVAL;
        return -1;
    }
    if (!buildCommandLine(argv, app, sizeof(app) / sizeof(app[0]), &cmd))
    {
        free(cmd.s);
        return -1;
    }
    pid = spawnWithHandles(app, cmd.s, stdin_fd, stdout_fd, stderr_fd, flags);
    free(cmd.s);
    return pid;
}

pid_t belugaSpawnShell(const char *command, const int stdin_fd, const int stdout_fd,
                       const int stderr_fd, const unsigned flags)
{
    wchar_t app[MAX_PATH * 2];
    wchar_t *wcommand = toWide(command);
    WBuf cmd = { NULL, 0, 0 };
    pid_t pid = -1;

    if (wcommand && comspec(app, sizeof(app) / sizeof(app[0])) &&
        wbufAppend(&cmd, L"cmd.exe /d /s /c \"") && wbufAppend(&cmd, wcommand) &&
        wbufAppend(&cmd, L"\""))
        pid = spawnWithHandles(app, cmd.s, stdin_fd, stdout_fd, stderr_fd, flags);
    else
        errno = ENOMEM;
    free(wcommand);
    free(cmd.s);
    return pid;
}

/***********************************************************************************************************************
 *
 *                                                 Terminal panes (ConPTY)
 *
 **********************************************************************************************************************/

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

/**
 * @brief Tears a terminal pane's pseudoconsole down.
 * @details ClosePseudoConsole() can wait for its last output to be read,
 * which nothing would do once the pane is gone, so it runs on a thread of
 * its own while the pipes are closed here.
 */
static void ptyTeardown(FdInfo *info)
{
    HANDLE thread;
    CloseHandle(info->pty_input);
    thread = CreateThread(NULL, 0, closePseudoConsoleThread, info->pty_console, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        ClosePseudoConsole(info->pty_console);
    info->is_pty = 0;
    info->pty_input = NULL;
    info->pty_console = NULL;
    info->pty_pid = 0;
}

int belugaPtySpawn(const char *shell, const int rows, const int cols, pid_t *pid_out)
{
    HANDLE in_read = NULL;
    HANDLE in_write = NULL;
    HANDLE out_read = NULL;
    HANDLE out_write = NULL;
    HPCON console = NULL;
    COORD size;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attr_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    WBuf cmd = { NULL, 0, 0 };
    wchar_t *wshell;
    wchar_t *env;
    pid_t pid = -1;
    int fd = -1;
    FdInfo *info;

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
    // Terminal and VS Code's node-pty do too. (Wine doesn't do that
    // fill-in, so terminal panes can't be tested under Wine.)
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;

    // Same TERM a Unix pane gets, for programs that look at it.
    env = environmentWith(L"TERM=xterm-256color");
    if (!CreateProcessW(NULL, cmd.s, NULL, NULL, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        env, NULL, &si.StartupInfo, &pi))
    {
        free(env);
        goto fail;
    }
    free(env);
    pid = adoptProcess(&pi);
    if (pid < 0)
        goto fail;

    // The pseudoconsole holds its own copies of these two.
    CloseHandle(in_read);
    CloseHandle(out_write);
    in_read = out_write = NULL;

    fd = _open_osfhandle((intptr_t) out_read, _O_RDONLY | _O_BINARY);
    info = fdInfo(fd);
    if (!info)
    {
        if (fd >= 0)
            _close(fd);
        else
            CloseHandle(out_read);
        out_read = NULL;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto fail;
    }
    memset(info, 0, sizeof(*info));
    info->is_pty = 1;
    info->pty_input = in_write;
    info->pty_console = console;
    info->pty_pid = pid;

    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    free(cmd.s);
    if (pid_out)
        *pid_out = pid;
    return fd;

fail:
    if (attrs)
    {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    free(cmd.s);
    if (console)
        ClosePseudoConsole(console);
    if (in_read) CloseHandle(in_read);
    if (in_write) CloseHandle(in_write);
    if (out_read) CloseHandle(out_read);
    if (out_write) CloseHandle(out_write);
    errno = EIO;
    return -1;
}

/***********************************************************************************************************************
 *
 *                                                 Console: raw mode and window size
 *
 **********************************************************************************************************************/

static DWORD s_orig_in_mode;
static DWORD s_orig_out_mode;
static int s_modes_saved;
static int s_vt_input; // the console accepted ENABLE_VIRTUAL_TERMINAL_INPUT

static HANDLE consoleIn(void) { return GetStdHandle(STD_INPUT_HANDLE); }
static HANDLE consoleOut(void) { return GetStdHandle(STD_OUTPUT_HANDLE); }

static int stdinIsConsole(void)
{
    DWORD mode;
    return GetConsoleMode(consoleIn(), &mode) != 0;
}

static void inputQueueClear(void);

int tcgetattr(const int fd, struct termios *t)
{
    (void) fd;
    if (!s_modes_saved)
    {
        if (!GetConsoleMode(consoleIn(), &s_orig_in_mode) ||
            !GetConsoleMode(consoleOut(), &s_orig_out_mode))
        {
            errno = ENOTTY;
            return -1;
        }
        s_modes_saved = 1;
    }
    memset(t, 0, sizeof(*t));
    t->c_iflag = BRKINT | ICRNL | IXON;
    t->c_oflag = OPOST;
    t->c_cflag = CS8;
    t->c_lflag = ISIG | ICANON | ECHO | IEXTEN;
    t->c_cc[VMIN] = 1;
    return 0;
}

int tcsetattr(const int fd, const int optional_actions, const struct termios *t)
{
    DWORD in_mode;
    DWORD out_mode;
    (void) fd;

    if (!s_modes_saved)
    {
        errno = ENOTTY;
        return -1;
    }

    if (t->c_lflag & ICANON)
    {
        // "Cooked": hand the console back exactly as we found it.
        SetConsoleMode(consoleIn(), s_orig_in_mode);
        SetConsoleMode(consoleOut(), s_orig_out_mode);
        return 0;
    }

    // Raw: keys as xterm byte sequences, no echo, no line editing, and
    // Ctrl-C delivered as a key (0x03) rather than as a signal. Quick-edit
    // (mouse selection in classic conhost) is left as the user set it.
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
        errno = ENOSYS;
        return -1;
    }

    if (optional_actions == TCSAFLUSH)
    {
        FlushConsoleInputBuffer(consoleIn());
        inputQueueClear();
    }
    return 0;
}

int beluga_ioctl(const int fd, const unsigned long request, ...)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    struct winsize *ws;
    FdInfo *fi;
    COORD size;
    va_list ap;

    va_start(ap, request);
    ws = va_arg(ap, struct winsize *);
    va_end(ap);

    if (request == TIOCGWINSZ)
    {
        if (!GetConsoleScreenBufferInfo(consoleOut(), &info))
        {
            errno = ENOTTY;
            return -1;
        }
        ws->ws_col = (unsigned short) (info.srWindow.Right - info.srWindow.Left + 1);
        ws->ws_row = (unsigned short) (info.srWindow.Bottom - info.srWindow.Top + 1);
        ws->ws_xpixel = ws->ws_ypixel = 0;
        return 0;
    }
    if (request == TIOCSWINSZ)
    {
        fi = fdInfo(fd);
        if (!fi || !fi->is_pty)
        {
            errno = ENOTTY;
            return -1;
        }
        size.X = (SHORT) ws->ws_col;
        size.Y = (SHORT) ws->ws_row;
        return SUCCEEDED(ResizePseudoConsole(fi->pty_console, size)) ? 0 : -1;
    }
    errno = EINVAL;
    return -1;
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

/**
 * @brief read() on the console: raw mode's VMIN=0/VTIME=1 behaviour — the
 * bytes already there, or the first to arrive within 100ms, or 0. The key
 * parser relies on that to tell a lone Escape from an escape sequence.
 */
static ssize_t consoleRead(void *buf, const size_t count)
{
    size_t n = 0;
    if (count == 0)
        return 0;
    if (s_inq_len == 0 && !waitForInput(100))
        return 0;
    while (n < count && s_inq_len > 0)
    {
        ((unsigned char *) buf)[n++] = s_inq[s_inq_head];
        s_inq_head = (s_inq_head + 1) % sizeof(s_inq);
        s_inq_len--;
    }
    return (ssize_t) n;
}

/***********************************************************************************************************************
 *
 *                                                 read / write / close / pipe / fcntl
 *
 **********************************************************************************************************************/

/**
 * @brief Bytes waiting in a pipe: >0 available, 0 none yet, -1 the writer
 * is gone (EOF) — or, for a terminal pane, the shell has exited.
 */
static long pipeAvailable(const int fd)
{
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    DWORD avail = 0;
    FdInfo *info = fdInfo(fd);

    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
        return -1;
    if (avail > 0)
        return (long) avail;
    // A pseudoconsole's output pipe stays open after its shell exits, so
    // EOF would never come: report it once the shell is gone instead.
    if (info && info->is_pty && processExited(info->pty_pid))
        return -1;
    return 0;
}

static int isPipe(const int fd)
{
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    return h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_PIPE;
}

ssize_t beluga_read(const int fd, void *buf, const size_t count)
{
    HANDLE h;
    long avail;
    DWORD got = 0;
    FdInfo *info = fdInfo(fd);

    if (fd == 0 && stdinIsConsole())
        return consoleRead(buf, count);
    if (!isPipe(fd))
        return _read(fd, buf, (unsigned int) count);

    avail = pipeAvailable(fd);
    if (avail < 0)
        return 0; // EOF
    if (avail == 0 && info && info->nonblock)
    {
        errno = EAGAIN;
        return -1;
    }

    // Blocking read, or data waiting (then ReadFile returns at once).
    h = (HANDLE) _get_osfhandle(fd);
    if (!ReadFile(h, buf, (DWORD) (avail > 0 && (size_t) avail < count ? (size_t) avail : count),
                  &got, NULL))
    {
        if (GetLastError() == ERROR_BROKEN_PIPE)
            return 0;
        errno = EIO;
        return -1;
    }
    return (ssize_t) got;
}

ssize_t beluga_write(const int fd, const void *buf, const size_t count)
{
    FdInfo *info = fdInfo(fd);
    DWORD written = 0;

    if (info && info->is_pty)
    {
        if (!WriteFile(info->pty_input, buf, (DWORD) count, &written, NULL))
        {
            errno = EPIPE;
            return -1;
        }
        return (ssize_t) written;
    }
    return _write(fd, buf, (unsigned int) count);
}

int beluga_close(const int fd)
{
    FdInfo *info = fdInfo(fd);
    if (info)
    {
        if (info->is_pty)
            ptyTeardown(info);
        memset(info, 0, sizeof(*info));
    }
    return _close(fd);
}

int beluga_pipe(int fds[2])
{
    // Not inheritable: belugaSpawn() hands children exactly the ends it
    // means to, so a stray copy can't keep a pipe from reaching EOF.
    if (_pipe(fds, 64 * 1024, _O_BINARY | _O_NOINHERIT) != 0)
        return -1;
    if (fdInfo(fds[0])) memset(fdInfo(fds[0]), 0, sizeof(FdInfo));
    if (fdInfo(fds[1])) memset(fdInfo(fds[1]), 0, sizeof(FdInfo));
    return 0;
}

int beluga_fcntl(const int fd, const int cmd, ...)
{
    FdInfo *info = fdInfo(fd);
    va_list ap;
    int arg;

    if (cmd == F_GETFL)
        return (info && info->nonblock) ? O_NONBLOCK : 0;
    if (cmd == F_SETFL)
    {
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
        if (info)
            info->nonblock = (arg & O_NONBLOCK) != 0;
        return 0;
    }
    return 0;
}

/***********************************************************************************************************************
 *
 *                                                 select / poll
 *
 **********************************************************************************************************************/

void beluga_fd_zero(beluga_fd_set *set) { set->count = 0; }

void beluga_fd_add(const int fd, beluga_fd_set *set)
{
    if (!beluga_fd_isset(fd, set) && set->count < BELUGA_FD_SETSIZE)
        set->fds[set->count++] = fd;
}

void beluga_fd_remove(const int fd, beluga_fd_set *set)
{
    int i;
    for (i = 0; i < set->count; i++)
        if (set->fds[i] == fd)
        {
            set->fds[i] = set->fds[--set->count];
            return;
        }
}

int beluga_fd_isset(const int fd, const beluga_fd_set *set)
{
    int i;
    for (i = 0; i < set->count; i++)
        if (set->fds[i] == fd)
            return 1;
    return 0;
}

/**
 * @brief Would read(fd) return without blocking? (Data, or EOF.)
 */
static int fdReadable(const int fd)
{
    if (fd == 0 && stdinIsConsole())
    {
        drainConsoleEvents();
        return s_inq_len > 0;
    }
    if (isPipe(fd))
        return pipeAvailable(fd) != 0;
    return 1; // files and the like never block
}

/**
 * @brief Waits for `fds` to become readable, for at most `timeout_ms`
 * (-1: forever). Fills `ready` (1 per readable fd); returns how many.
 * @details The console can be waited on, pipes can't (only peeked), so
 * with pipes in the set it waits on the console in slices of 10ms and
 * peeks the pipes in between.
 */
static int waitReadable(const int *fds, const int n, int *ready, const long timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + (timeout_ms < 0 ? 0 : (ULONGLONG) timeout_ms);
    ULONGLONG now;
    DWORD slice;
    int watch_console = 0;
    int has_pipes = 0;
    int count;
    int i;

    for (i = 0; i < n; i++)
    {
        if (fds[i] == 0 && stdinIsConsole())
            watch_console = 1;
        else
            has_pipes = 1;
    }

    for (;;)
    {
        count = 0;
        for (i = 0; i < n; i++)
        {
            ready[i] = fdReadable(fds[i]);
            count += ready[i];
        }
        if (count > 0 || timeout_ms == 0)
            return count;

        now = GetTickCount64();
        if (timeout_ms > 0 && now >= deadline)
            return 0;
        slice = timeout_ms < 0 ? INFINITE : (DWORD) (deadline - now);
        if (has_pipes && slice > 10)
            slice = 10;

        if (watch_console)
            WaitForSingleObject(consoleIn(), slice);
        else
            Sleep(slice == INFINITE ? 10 : slice);
    }
}

int beluga_select(const int nfds, beluga_fd_set *readfds, beluga_fd_set *writefds,
                  beluga_fd_set *exceptfds, struct timeval *timeout)
{
    int fds[BELUGA_FD_SETSIZE];
    int ready[BELUGA_FD_SETSIZE];
    long timeout_ms = -1;
    int n = 0;
    int count;
    int i;
    (void) nfds;

    if (writefds) writefds->count = 0;
    if (exceptfds) exceptfds->count = 0;
    if (timeout)
        timeout_ms = (long) timeout->tv_sec * 1000 + (long) timeout->tv_usec / 1000;
    if (readfds)
    {
        n = readfds->count;
        memcpy(fds, readfds->fds, (size_t) n * sizeof(int));
    }

    count = waitReadable(fds, n, ready, timeout_ms);
    if (readfds)
    {
        readfds->count = 0;
        for (i = 0; i < n; i++)
            if (ready[i])
                readfds->fds[readfds->count++] = fds[i];
    }
    return count;
}

int beluga_poll(struct pollfd *pfds, const nfds_t nfds, const int timeout_ms)
{
    int fds[BELUGA_FD_SETSIZE];
    int ready[BELUGA_FD_SETSIZE];
    int n = nfds < BELUGA_FD_SETSIZE ? (int) nfds : BELUGA_FD_SETSIZE;
    int count;
    int i;

    for (i = 0; i < n; i++)
        fds[i] = pfds[i].fd;
    count = waitReadable(fds, n, ready, timeout_ms);
    for (i = 0; i < n; i++)
    {
        pfds[i].revents = 0;
        if (ready[i])
            pfds[i].revents = (isPipe(fds[i]) && pipeAvailable(fds[i]) < 0) ? POLLHUP : POLLIN;
    }
    return count;
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

char *belugaClipboardGet(void)
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

int belugaClipboardSet(const char *text, const size_t len)
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

/***********************************************************************************************************************
 *
 *                                                 Paths, environment, libc gaps
 *
 **********************************************************************************************************************/

char *beluga_getcwd(char *buf, const size_t size)
{
    char *r = _getcwd(buf, (int) size);
    backslashesToSlashes(r);
    return r;
}

/**
 * @brief realpath(): the absolute, canonical path of an existing file,
 * with forward slashes.
 * @details GetFinalPathNameByHandle resolves links and corrects the
 * case, so "Main.C" and "main.c" open as the same buffer, as they are
 * the same file on Windows. Fails (ENOENT) for a path that doesn't exist,
 * like realpath() does.
 */
char *beluga_realpath(const char *path, char *resolved)
{
    wchar_t *wpath = toWide(path);
    wchar_t final[PATH_MAX];
    const wchar_t *p = final;
    HANDLE h;
    DWORD n;
    char *utf8;

    if (!wpath)
    {
        errno = ENOENT;
        return NULL;
    }
    h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = ENOENT;
        return NULL;
    }
    n = GetFinalPathNameByHandleW(h, final, PATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }

    // \\?\C:\dir -> C:\dir ; \\?\UNC\server\share -> \\server\share
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0)
    {
        p += 6;
        final[6] = L'\\';
    }
    else if (wcsncmp(p, L"\\\\?\\", 4) == 0)
        p += 4;

    utf8 = toUtf8(p);
    if (!utf8)
    {
        errno = ENOMEM;
        return NULL;
    }
    backslashesToSlashes(utf8);
    if (!resolved)
        return utf8; // malloc'd, as POSIX realpath(path, NULL) does
    snprintf(resolved, PATH_MAX, "%s", utf8);
    free(utf8);
    return resolved;
}

int beluga_setenv(const char *name, const char *value, const int overwrite)
{
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

int beluga_unsetenv(const char *name)
{
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

beluga_sighandler_t beluga_signal(const int sig, const beluga_sighandler_t handler)
{
    switch (sig)
    {
    case SIGINT: case SIGILL: case SIGFPE: case SIGSEGV: case SIGTERM: case SIGABRT:
#ifdef SIGBREAK
    case SIGBREAK:
#endif
        return signal(sig, handler); // not redirected here: the CRT's own
    default:
        return SIG_DFL;
    }
}

ssize_t beluga_getline(char **lineptr, size_t *n, FILE *stream)
{
    size_t len = 0;
    char *grown;
    int c;

    if (!lineptr || !n || !stream)
    {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || *n == 0)
    {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr)
            return -1;
    }
    while ((c = fgetc(stream)) != EOF)
    {
        if (len + 2 > *n)
        {
            grown = realloc(*lineptr, *n * 2);
            if (!grown)
                return -1;
            *lineptr = grown;
            *n *= 2;
        }
        (*lineptr)[len++] = (char) c;
        if (c == '\n')
            break;
    }
    (*lineptr)[len] = '\0';
    return len == 0 ? -1 : (ssize_t) len;
}

void *beluga_memmem(const void *haystack, const size_t haystack_len, const void *needle,
                    const size_t needle_len)
{
    const unsigned char *h = haystack;
    size_t i;
    if (needle_len == 0)
        return (void *) haystack;
    for (i = 0; i + needle_len <= haystack_len; i++)
        if (h[i] == *(const unsigned char *) needle && memcmp(h + i, needle, needle_len) == 0)
            return (void *) (h + i);
    return NULL;
}

char *beluga_strndup(const char *s, const size_t n)
{
    size_t len = 0;
    char *copy;
    while (len < n && s[len])
        len++;
    copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

size_t beluga_strftime(char *s, const size_t max, const char *format, const struct tm *tm)
{
    char fixed[256];
    size_t i = 0;
    const char *p;

    for (p = format; *p && i + 2 < sizeof(fixed); p++)
    {
        if (p[0] == '%' && p[1] == 'e')
        {
            fixed[i++] = '%';
            fixed[i++] = 'd';
            p++;
        }
        else
            fixed[i++] = *p;
    }
    fixed[i] = '\0';
    return strftime(s, max, fixed, tm); // the CRT's own: not redirected here
}

/***********************************************************************************************************************
 *
 *                                                 File operations (dired)
 *
 **********************************************************************************************************************/

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
    wchar_t full[PATH_MAX];
    wchar_t *w;
    wchar_t *p;
    const wchar_t nul = L'\0';
    int i;

    for (i = 0; i < count; i++)
    {
        w = toWide(paths[i]);
        if (!w || !GetFullPathNameW(w, PATH_MAX, full, NULL))
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

int belugaFileOperation(const int op, const char *const *paths, const int count,
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
    if (op != BELUGA_FILEOP_DELETE)
        to = dest ? doubleNulList(&dest, 1) : NULL;
    if (!from || (op != BELUGA_FILEOP_DELETE && !to))
    {
        free(from);
        free(to);
        snprintf(err, err_size, "invalid path");
        return ERROR_INVALID_NAME;
    }

    memset(&fo, 0, sizeof(fo));
    fo.wFunc = op == BELUGA_FILEOP_DELETE ? FO_DELETE : op == BELUGA_FILEOP_MOVE ? FO_MOVE : FO_COPY;
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

int belugaMakeDirectories(const char *path, char *err, const size_t err_size)
{
    wchar_t *w = toWide(path);
    wchar_t full[PATH_MAX];
    wchar_t *p;
    int result;

    if (err && err_size)
        err[0] = '\0';
    if (!w || !GetFullPathNameW(w, PATH_MAX, full, NULL))
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

/***********************************************************************************************************************
 *
 *                                                 PowerShell
 *
 **********************************************************************************************************************/

const char *belugaPowerShell(void)
{
    static const char *found = NULL;
    wchar_t path[MAX_PATH * 2];
    int is_batch;
    if (!found)
        found = resolveProgram("pwsh", path, MAX_PATH * 2, &is_batch) ? "pwsh" : "powershell";
    return found;
}

char *belugaPowerShellEncode(const char *script)
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
