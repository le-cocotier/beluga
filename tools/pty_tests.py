#!/usr/bin/env python3
"""Headless tests that look at beluga's screen: the editor runs in a
pseudo-terminal (tools/pty_harness.py) and pyte emulates the terminal.

    tools/pty_tests.py BELUGA_BINARY REPO_DIR

POSIX only. Exits 77 (meson's "skipped") when pyte isn't installed
(python3-pyte, or pip install pyte). A test that needs a program the
machine doesn't have (less, vim) is skipped on its own.
"""
import os
import re
import shutil
import signal
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pty_harness  # noqa: E402
from pty_harness import Beluga, run_tests  # noqa: E402

CTRL_X = '\x18'
DOWN = '\x1b[B'
DEBUG = (CTRL_X, 'B')  # the debugger's prefix: DEBUG + ('b',) toggles a breakpoint

DEBUGGEE = """#include <stdio.h>
static int square(int v) {
  int r = v * v;
  return r;
}
int main(void) {
  int i, total = 0;
  for (i = 0; i < 3; i++)
    total += square(i + 3);
  printf("total %d\\n", total);
  return total % 7;
}
"""


class Skip(Exception):
    pass


def open_terminal(ed):
    """CTRL-x t, then waits for the shell's prompt."""
    ed.keys(CTRL_X, 't', delay=0.5)
    ed.wait_for('$')


def shell(ed, command):
    ed.write(command + '\r')


def tab_completion_list(binary, repo):
    """Several matches: the panel lists them, and TAB steps through them
    with the one in the prompt highlighted."""
    with Beluga(binary, repo) as ed:
        for name in ('alpha1.txt', 'alpha2.txt', 'alpha3.txt', 'beta.txt'):
            with open(ed.path(name), 'w') as f:
                f.write('x\n')
        ed.start('beta.txt')
        ed.keys(CTRL_X, 'f', delay=0.4)
        ed.keys('a', 'l', 'p', '\t', delay=0.3)
        ed.wait_for('3 matches (TAB to cycle)')
        ed.wait_for('alpha1.txt        alpha2.txt        alpha3.txt')
        ed.pump(0.5)  # a pause between TABs must not break the cycle
        ed.keys('\t')
        ed.wait_for('1/3 matches')
        ed.keys('\t')
        ed.wait_for('2/3 matches')
        prompt = [line for line in ed.text() if line.startswith('Open :')]
        assert prompt and prompt[0].endswith('/alpha2.txt'), prompt
        y = next(i for i, line in enumerate(ed.screen.display)
                 if 'alpha1.txt' in line and 'alpha2.txt' in line)
        x1 = ed.screen.display[y].index('alpha1.txt')
        x2 = ed.screen.display[y].index('alpha2.txt')
        row = ed.screen.buffer[y]
        assert (row[x2].bg, row[x2].bold) != (row[x1].bg, row[x1].bold), \
            'the selected match is not highlighted'
        # Typing makes the list stale: it goes away.
        ed.keys('x', delay=0.3)
        assert not any('matches' in line for line in ed.text()), ed.text()
        ed.keys('\x1b', delay=0.3)
        ed.quit()


def terminal_alternate_screen(binary, repo):
    """less draws on the alternate screen and pages with scrolling; on
    quitting, the shell's screen comes back as it was."""
    if not shutil.which('less'):
        raise Skip('no less')
    with Beluga(binary, repo) as ed:
        with open(ed.path('long.txt'), 'w') as f:
            f.write(''.join('line %d\n' % i for i in range(1, 201)))
        ed.start()
        open_terminal(ed)
        shell(ed, 'echo before-less')
        ed.wait_for('before-less', 5)
        shell(ed, 'LESS= less long.txt')
        ed.wait_for('line 20')
        assert not any('before-less' in line for line in ed.text()), \
            'the alternate screen is not blank'
        ed.keys(' ', delay=0.5)      # a page down
        ed.wait_for('line 40')
        ed.keys('k', delay=0.5)      # one line up: scrolls the screen down
        top = ed.text()[0]
        ed.keys('j', delay=0.5)      # and down again
        assert ed.text()[0] != top, 'less did not scroll'
        ed.keys('q', delay=0.5)
        ed.wait_for('before-less')
        assert not any('line 40' in line for line in ed.text()), \
            "less's screen is still there:\n" + '\n'.join(ed.text())
        ed.quit()


def terminal_vim(binary, repo):
    """vim (scroll regions, insert/delete line, the alternate screen):
    what it shows matches the file, and editing it works."""
    vim = shutil.which('vim') or shutil.which('vim.tiny')
    if not vim:
        raise Skip('no vim')
    with Beluga(binary, repo) as ed:
        with open(ed.path('v.txt'), 'w') as f:
            f.write(''.join('row %02d\n' % i for i in range(1, 61)))
        ed.start()
        open_terminal(ed)
        shell(ed, '%s -u NONE -N -i NONE v.txt' % vim)
        ed.wait_for('row 01')
        ed.keys('G', delay=0.5)     # to the end: the screen scrolls
        ed.wait_for('row 60')
        ed.keys('\x15', delay=0.5)  # CTRL-u: half a page up, scrolling down
        rows = [line for line in ed.text() if line.startswith('row ')]
        numbers = [int(line[4:6]) for line in rows]
        assert len(rows) > 20 and numbers == list(range(numbers[0], numbers[0] + len(rows))), \
            'the screen is not the file:\n' + '\n'.join(ed.text())
        ed.keys('g', 'g', delay=0.3)
        ed.wait_for('row 01')
        ed.keys('d', 'd', delay=0.3)  # delete line: the rest moves up
        lines = ed.text()
        first = next(i for i, line in enumerate(lines) if 'row 02' in line)
        assert 'row 03' in lines[first + 1], lines
        # Insert a line above. CTRL-c leaves insert mode: ESC is the
        # terminal pane's own key (back to the editor, config/init.lisp).
        ed.keys('O', 'n', 'e', 'w', '\x03', delay=0.2)
        ed.wait_for('new')
        ed.keys(':', 'w', 'q', '\r', delay=0.5)
        ed.wait_for('$ ')
        with open(ed.path('v.txt')) as f:
            text = f.read().splitlines()
        assert text[:2] == ['new', 'row 02'], text[:3]
        ed.quit()


def terminal_colors(binary, repo):
    """256-colour and truecolour SGR reach the screen."""
    with Beluga(binary, repo) as ed:
        ed.start()
        open_terminal(ed)
        shell(ed, r"printf '\033[38;5;208mORANGE\033[0m \033[48;2;10;20;30mDEEP\033[0m\n'")
        ed.wait_for('ORANGE DEEP')
        y = next(i for i, line in enumerate(ed.screen.display)
                 if 'ORANGE DEEP' in line and 'printf' not in line)
        x = ed.screen.display[y].index('ORANGE')
        assert ed.screen.buffer[y][x].fg == 'ff8700', ed.screen.buffer[y][x]
        x = ed.screen.display[y].index('DEEP')
        assert ed.screen.buffer[y][x].bg == '0a141e', ed.screen.buffer[y][x]
        ed.quit()


def terminal_wrap(binary, repo):
    """Output wider than the pane wraps onto the next line instead of
    being cut at the pane's edge, and a line that exactly fills it leaves
    no blank line after it."""
    with Beluga(binary, repo) as ed:
        ed.start()
        open_terminal(ed)
        shell(ed, "printf 'W%.0s' $(seq 1 130); echo; "
                  "printf 'F%.0s' $(seq 1 100); echo; echo end")
        ed.wait_for('end')
        lines = ed.text()
        y = next(i for i, line in enumerate(lines) if line.startswith('WWW'))
        assert lines[y] == 'W' * 100 and lines[y + 1] == 'W' * 30, lines[y:y + 2]
        assert lines[y + 2] == 'F' * 100 and lines[y + 3] == 'end', lines[y + 2:y + 4]
        ed.quit()


def terminal_wide_characters(binary, repo):
    """The shell moving the cursor on a line with wide characters lands
    where the terminal shows it."""
    with Beluga(binary, repo) as ed:
        ed.start()
        open_terminal(ed)
        # Two wide characters, back to the line's start, then 5 columns on.
        shell(ed, "printf '\\344\\270\\255\\346\\226\\207abc\\r\\033[5CX\\n'")
        ed.wait_for('中文aXc')
        ed.quit()


def terminal_fast_typing(binary, repo):
    """A line typed into the terminal pane in one burst arrives whole (the
    keystroke loss in TODO.md doesn't reproduce here)."""
    with Beluga(binary, repo) as ed:
        ed.start()
        open_terminal(ed)
        shell(ed, 'cat > typed.txt')
        ed.pump(0.5)
        line = ''.join(chr(ord('a') + i % 26) for i in range(100))
        ed.write(line + '\r')
        ed.pump(0.5)
        for ch in line[:40]:        # and one key at a time, 2 ms apart
            ed.write(ch)
            ed.pump(0.002)
        ed.write('\r\x04')
        ed.pump(1.0)
        with open(ed.path('typed.txt')) as f:
            got = f.read()
        assert got == line + '\n' + line[:40] + '\n', repr(got)
        ed.quit()


def terminal_long_burst(binary, repo):
    """A 2000-character command line sent in one write reaches the shell
    whole: more than macOS's ~1 KB pty input queue between the harness and
    the editor. (Not into `cat`: a canonical-mode line longer than 1024
    bytes is discarded by the macOS tty itself, editor or not.)"""
    with Beluga(binary, repo) as ed:
        ed.start()
        open_terminal(ed)
        line = ''.join(chr(ord('a') + i % 26) for i in range(2000))
        shell(ed, 'echo ' + line + ' > long.txt')
        ed.pump(2.0)
        with open(ed.path('long.txt')) as f:
            got = f.read()
        assert got == line + '\n', '%d bytes: %r' % (len(got), got[:80])
        ed.quit()


def breakpoint_gutter(binary, repo):
    """CTRL-x B b puts a breakpoint in the gutter; it moves down with a
    line inserted above it, and CTRL-x B b again takes it away."""
    with Beluga(binary, repo) as ed:
        with open(ed.path('t.c'), 'w') as f:
            f.write(DEBUGGEE)
        ed.start('t.c')
        ed.wait_for('square')
        ed.keys(DOWN, DOWN, *DEBUG, 'b')
        ed.wait_for('Added breakpoint at line 3')
        assert re.match(r'\s*3 ●', ed.text()[2]), ed.text()[2]
        ed.keys('\x1b[A', '\x1b[A', '\r', delay=0.2)  # a line above it
        ed.pump(0.3)
        assert re.match(r'\s*4 ●', ed.text()[3]), ed.text()[:5]
        ed.keys(DOWN, DOWN, *DEBUG, 'b')
        ed.wait_for('Removed breakpoint at line 4')
        assert '●' not in ''.join(ed.text())
        ed.keys(CTRL_X, 'u', delay=0.3)  # undo the new line, quit unsaved
        ed.quit()


def gdb_with_dap():
    """gdb 14 or later (it speaks the Debug Adapter Protocol), or None."""
    gdb = shutil.which('gdb')
    if not gdb or not shutil.which('cc'):
        return None
    out = subprocess.run([gdb, '--version'], capture_output=True, text=True).stdout
    m = re.search(r'(\d+)\.\d+', out.splitlines()[0] if out else '')
    return gdb if m and int(m.group(1)) >= 14 else None


def debug_session(binary, repo, config):
    """A debug session with the adapter `config` (Lisp for personal-config)
    sets up: stop at a breakpoint, see the stack and the variables, step,
    evaluate, run to the end. The files are reached through HOME, which on
    macOS is under /var, a link to /private/var."""
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        with open(ed.path('t.c'), 'w') as f:
            f.write(DEBUGGEE)
        subprocess.run(['cc', '-g', '-O0', '-o', ed.path('t'), ed.path('t.c')],
                       check=True)
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n' + config + '\n')
        ed.start('t.c')
        ed.wait_for('square')
        ed.keys(DOWN, DOWN, *DEBUG, 'b')
        ed.keys(*DEBUG, 'c')
        ed.wait_for('Debug program:')
        ed.write(ed.path('t') + '\r')
        ed.wait_for('Arguments:')
        ed.write('\r')
        ed.wait_for('stopped (breakpoint)', 20)
        ed.wait_for('v = 3  (int)')
        assert re.match(r'\s*3 ▶', ed.text()[2]), ed.text()[2]
        assert any('#1 main  t.c:9' in line for line in ed.text()), ed.text()
        ed.keys(*DEBUG, 'n')
        ed.wait_for('r = 9  (int)')
        assert re.match(r'\s*4 ▶', ed.text()[3]), ed.text()[3]
        ed.keys(*DEBUG, 'e', delay=0.3)
        ed.write('v * 10\r')
        ed.wait_for('v * 10 = 30')
        # The cursor followed the program to line 4: back up to the
        # breakpoint and take it away, so the program runs to its end.
        ed.keys('\x1b[A', *DEBUG, 'b')
        ed.keys(*DEBUG, 'c')
        ed.wait_for('the program exited with code 1')
        assert any('total 50' in line for line in ed.text()), ed.text()
        # The breakpoint was found at once, not left pending until the
        # program loaded (gdb gets the program on its command line).
        ed.keys(*DEBUG, 'o', delay=0.5)
        ed.wait_for('total 50')
        assert not any('No source file' in line for line in ed.text()), ed.text()
        ed.quit()


def debugger_gdb(binary, repo):
    """The session against gdb -i dap."""
    if sys.platform == 'darwin':
        raise Skip('gdb on macOS needs code signing to run programs')
    if not gdb_with_dap():
        raise Skip('needs gdb 14 or later and cc')
    debug_session(binary, repo, '(dap-adapter "c" "gdb" "-q" "-i" "dap")')


def debugger_lldb(binary, repo):
    """The session against lldb-dap, as packages/c sets it up on macOS."""
    if sys.platform != 'darwin' or not shutil.which('cc'):
        raise Skip('macOS only')
    if subprocess.run(['xcrun', '-f', 'lldb-dap'], capture_output=True).returncode != 0:
        raise Skip('no lldb-dap (Xcode command line tools)')
    debug_session(binary, repo, '(define LSP #f)\n(add-package "c")')


def terminal_app_shift_function_keys(binary, repo):
    """Terminal.app sends Shift+F5-F12 as the VT220's F13-F20 codes
    (ESC [25~ ... ESC [34~); xterm and iTerm2 send ESC [15;2~."""
    with Beluga(binary, repo) as ed:
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(map-key "SHIFT-F5" \'(editor-insert-char "A") "no-prefix")'
                    '\n(map-key "SHIFT-F11" \'(editor-insert-char "B") "no-prefix")'
                    '\n(map-key "SHIFT-F12" \'(editor-insert-char "C") "no-prefix")\n')
        with open(ed.path('k.txt'), 'w') as f:
            f.write('\n')
        ed.start('k.txt')
        ed.keys('\x1b[25~', '\x1b[33~', '\x1b[34~', '\x1b[15;2~', delay=0.3)
        ed.wait_for('ABCA')
        ed.keys(CTRL_X, 'u', CTRL_X, 'u', CTRL_X, 'u', CTRL_X, 'u', delay=0.2)
        ed.quit()


def daemon_name(tag):
    return 'test-%s-%d' % (tag, os.getpid())


def daemon_in_background(binary, repo):
    """beluga --daemon returns at once, the daemon going on in the
    background; a second one is refused; -c opens a window on it."""
    name = daemon_name('bg')
    with Beluga(binary, repo) as ed:
        try:
            rc, out = ed.run('--daemon=' + name)
            assert rc == 0 and 'started' in out, (rc, out)
            rc, out = ed.run('--daemon=' + name)
            assert rc == 1 and 'already running' in out, (rc, out)
            with open(ed.path('a.txt'), 'w') as f:
                f.write('alpha\n')
            ed.start('-c', '--name=' + name, 'a.txt')
            ed.wait_for('alpha')
            ed.quit()
        finally:
            rc, out = ed.run('--kill', '--force', '--name=' + name)
        assert rc == 0, out


def daemon_started_by_client(binary, repo):
    """beluga -c with no daemon running starts one."""
    name = daemon_name('auto')
    with Beluga(binary, repo) as ed:
        try:
            ed.start('-c', '--name=' + name)
            ed.wait_for('closes this window')
            ed.quit()
            rc, out = ed.run('--kill', '--name=' + name)
            assert rc == 0, out
        finally:
            ed.run('--kill', '--force', '--name=' + name)


def daemon_window_directory(binary, repo):
    """Each daemon window works from the directory its beluga -c ran in:
    the open-file prompt starts there, and so does a terminal pane."""
    name = daemon_name('cwd')
    with Beluga(binary, repo) as ed:
        try:
            os.makedirs(ed.path('proj/sub'))
            rc, out = ed.run('--daemon=' + name)
            assert rc == 0, out
            ed.start('-c', '--name=' + name, cwd='proj/sub')
            ed.wait_for('closes this window')
            ed.keys(CTRL_X, 'f', delay=0.4)
            ed.wait_for('proj/sub/')
            ed.keys('\x1b', delay=0.3)  # no file after all
            open_terminal(ed)
            shell(ed, 'pwd')
            ed.wait_for(ed.path('proj/sub'))
            ed.quit()
        finally:
            ed.run('--kill', '--force', '--name=' + name)


def daemon_stop_signals(binary, repo):
    """Ctrl-C on a daemon with an unsaved buffer warns; a second one stops
    it, keeping the changes in autosave and taking the socket away. A
    closed terminal (SIGHUP) stops one at once."""
    name = daemon_name('sig')
    with Beluga(binary, repo) as daemon:
        try:
            with open(daemon.path('u.txt'), 'w') as f:
                f.write('text\n')
            daemon.start('--daemon=' + name, '--foreground')
            daemon.wait_for('daemon "%s" running' % name)
            with Beluga(binary, repo, home=daemon.home) as window:
                window.start('-c', '--name=' + name, 'u.txt')
                window.wait_for('text')
                window.keys('X', delay=0.3)  # unsaved now
                window.quit()                # the window only
            os.kill(daemon.pid, signal.SIGINT)
            daemon.wait_for('press Ctrl-C again')
            assert daemon.status is None and os.waitpid(daemon.pid, os.WNOHANG)[0] == 0, \
                'one Ctrl-C stopped it'
            os.kill(daemon.pid, signal.SIGINT)
            status = daemon.wait_exit(10)
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
            daemon.wait_for('~/.beluga/autosave')
            saved = os.listdir(daemon.path('.beluga/autosave'))
            assert any(entry.endswith('u.txt') for entry in saved), saved
            rc, out = daemon.run('--kill', '--name=' + name)
            assert rc == 1, 'still reachable: ' + out
        finally:
            daemon.run('--kill', '--force', '--name=' + name)
    with Beluga(binary, repo) as daemon:
        try:
            daemon.start('--daemon=' + name, '--foreground')
            daemon.wait_for('running')
            os.kill(daemon.pid, signal.SIGHUP)
            status = daemon.wait_exit(10)
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
        finally:
            daemon.run('--kill', '--force', '--name=' + name)


TESTS = [
    ('breakpoint-gutter', breakpoint_gutter),
    ('debugger-gdb', debugger_gdb),
    ('debugger-lldb', debugger_lldb),
    ('terminal-app-shift-function-keys', terminal_app_shift_function_keys),
    ('tab-completion-list', tab_completion_list),
    ('terminal-alternate-screen', terminal_alternate_screen),
    ('terminal-vim', terminal_vim),
    ('terminal-colors', terminal_colors),
    ('terminal-wide-characters', terminal_wide_characters),
    ('terminal-wrap', terminal_wrap),
    ('terminal-fast-typing', terminal_fast_typing),
    ('daemon-in-background', daemon_in_background),
    ('daemon-started-by-client', daemon_started_by_client),
    ('daemon-window-directory', daemon_window_directory),
    ('daemon-stop-signals', daemon_stop_signals),
    ('terminal-long-burst', terminal_long_burst),
]


def main():
    if pty_harness.pyte is None:
        print('pty tests skipped: pyte is not installed (python3-pyte)')
        return 77
    tests = []
    for name, test in TESTS:
        def run(binary, repo, test=test, name=name):
            try:
                test(binary, repo)
            except Skip as e:
                print('skip  %s: %s' % (name, e))
        tests.append((name, run))
    return run_tests(tests, sys.argv[1], sys.argv[2])


if __name__ == '__main__':
    sys.exit(main())
