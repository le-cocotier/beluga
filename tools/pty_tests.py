#!/usr/bin/env python3
"""Headless tests that look at beluga's screen: the editor runs in a
pseudo-terminal (tools/pty_harness.py) and pyte emulates the terminal.

    tools/pty_tests.py BELUGA_BINARY REPO_DIR [TEST...]
    tools/pty_tests.py --list

POSIX only. Exits 77 (meson's "skipped") when pyte isn't installed
(python3-pyte, or pip install pyte). A test that needs a program the
machine doesn't have (less, vim) is skipped on its own.
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pty_harness  # noqa: E402
from pty_harness import Beluga, Skip, run_tests  # noqa: E402

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
        got = ''
        for _ in range(100):  # slower under a sanitizer, or a busy machine
            ed.pump(0.1)
            if os.path.exists(ed.path('long.txt')):
                with open(ed.path('long.txt')) as f:
                    got = f.read()
                if got.endswith('\n'):
                    break
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


def pane_cursors(binary, repo):
    """Two panes on the same file each keep their own cursor: moving in one
    doesn't move the other, and lines inserted above a pane's cursor carry
    it down with its line."""
    with Beluga(binary, repo, rows=30, cols=100) as ed:
        with open(ed.path('p.txt'), 'w') as f:
            f.write(''.join('line %d\n' % i for i in range(1, 41)))
        ed.start('p.txt')
        ed.wait_for('line 20')
        ed.keys(CTRL_X, '"', delay=0.4)      # split: the new pane has focus
        ed.keys(*([DOWN] * 5))               # its cursor to line 6
        ed.keys(CTRL_X, 'o', delay=0.3)      # the first pane: still on line 1
        ed.keys('A', '\r', delay=0.2)       # a line inserted above line 6
        ed.keys(CTRL_X, 'o', delay=0.3)
        ed.keys('B', delay=0.3)
        lines = ed.text()
        assert lines[0].split('│')[0].rstrip().endswith('A'), lines[:3]
        assert any('Bline 6' in line for line in lines), lines[:10]
        assert not any('Bline' in line and 'Bline 6' not in line for line in lines), lines[:10]
        for _ in range(3):
            ed.keys(CTRL_X, 'u', delay=0.2)
        ed.quit()


MOUSE_MODE = 1000 << 5  # pyte keeps private modes shifted left by 5


def wheel(row, col, up=False):
    """An SGR wheel notch at 1-based screen cell (row, col)."""
    return '\x1b[<%d;%d;%dM' % (64 if up else 65, col, row)


def click(row, col):
    """An SGR left press and release at 1-based screen cell (row, col)."""
    return ('\x1b[<0;%d;%dM' % (col, row), '\x1b[<0;%d;%dm' % (col, row))


def mouse_wheel_and_click(binary, repo):
    """The editor asks for mouse reports; a wheel notch scrolls the view 3
    lines with the cursor kept on screen; a click past a line's end puts
    the cursor at its end, one on the gutter at its start; nothing of a
    report is ever typed. Quitting turns the reports off again."""
    with Beluga(binary, repo) as ed:
        with open(ed.path('m.txt'), 'w') as f:
            f.write(''.join('line %d\n' % i for i in range(1, 101)))
        ed.start('m.txt')
        ed.wait_for('line 20')
        assert MOUSE_MODE in ed.screen.mode, sorted(ed.screen.mode)
        ed.keys(wheel(5, 20), wheel(5, 20), delay=0.3)
        lines = ed.text()
        assert lines[0].split()[-1] == '7', lines[:3]
        assert ed.screen.cursor.y == 0, ed.screen.cursor.y  # rode along on line 7
        ed.keys(wheel(5, 20, up=True), delay=0.3)
        assert ed.text()[0].split()[-1] == '4', ed.text()[:3]
        ed.keys(*click(10, 90), 'X', delay=0.3)  # row 10 shows line 13
        assert ed.text()[9].endswith('line 13X'), ed.text()[9]
        ed.keys(*click(12, 1), 'Y', delay=0.3)
        assert 'Yline 15' in ed.text()[11], ed.text()[11]
        assert not any('65;' in line or '<0' in line for line in ed.text()), ed.text()
        ed.keys(CTRL_X, 'u', delay=0.2)
        ed.keys(CTRL_X, 'u', delay=0.2)
        ed.quit()
        ed.pump(0.2)
        assert MOUSE_MODE not in ed.screen.mode, sorted(ed.screen.mode)


def mouse_split_panes(binary, repo):
    """A click in the other pane gives it focus and its cursor goes where
    clicked; the wheel scrolls the pane under the pointer only."""
    with Beluga(binary, repo, rows=30, cols=100) as ed:
        with open(ed.path('p.txt'), 'w') as f:
            f.write(''.join('line %d\n' % i for i in range(1, 101)))
        ed.start('p.txt')
        ed.wait_for('line 20')
        ed.keys(CTRL_X, '"', delay=0.4)        # side by side, right one focused
        ed.keys(*click(5, 30), 'L', delay=0.3)  # left pane, line 5
        assert ed.text()[4].split('│')[0].rstrip().endswith('line 5L'), ed.text()[4]
        assert ed.screen.cursor.x < 50, ed.screen.cursor.x
        ed.keys(wheel(5, 80), delay=0.3)       # the right pane scrolls
        left, right = ed.text()[0].split('│')
        assert left.split()[-1] == '1' and right.split()[-1] == '4', ed.text()[0]
        ed.keys(CTRL_X, 'u', delay=0.2)
        ed.quit()


def daemon_mouse(binary, repo):
    """A daemon window gets mouse reports too: the request goes out with
    what the daemon draws, and the client turns it off when it closes."""
    name = daemon_name('mouse')
    with Beluga(binary, repo) as ed:
        try:
            with open(ed.path('m.txt'), 'w') as f:
                f.write(''.join('line %d\n' % i for i in range(1, 101)))
            ed.start('-c', '--name=' + name, 'm.txt')
            ed.wait_for('line 20')
            assert MOUSE_MODE in ed.screen.mode, sorted(ed.screen.mode)
            ed.keys(wheel(5, 20), delay=0.4)
            assert ed.text()[0].split()[-1] == '4', ed.text()[:3]
            ed.quit()
            ed.pump(0.2)
            assert MOUSE_MODE not in ed.screen.mode, sorted(ed.screen.mode)
        finally:
            ed.run('--kill', '--force', '--name=' + name)


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


ATTACHEE = """#include <stdio.h>
#include <unistd.h>
static volatile int ticks;
static void tick(void) {
  ticks++;
}
int main(void) {
  printf("ready\\n");
  fflush(stdout);
  for (;;) {
    tick();
    usleep(50000);
  }
}
"""


def debugger_attach(binary, repo):
    """CTRL-x B a lists the running processes; picking one attaches gdb to
    it, its breakpoints work, and stopping detaches, the program going on."""
    if sys.platform == 'darwin':
        raise Skip('gdb on macOS needs code signing to run programs')
    if not gdb_with_dap():
        raise Skip('needs gdb 14 or later and cc')
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        with open(ed.path('t.c'), 'w') as f:
            f.write(ATTACHEE)
        subprocess.run(['cc', '-g', '-O0', '-o', ed.path('t'), ed.path('t.c')],
                       check=True)
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(dap-adapter "c" "gdb" "-q" "-i" "dap")\n')
        program = subprocess.Popen([ed.path('t')], stdout=subprocess.PIPE)
        try:
            assert program.stdout.readline() == b'ready\n'
            ed.start('t.c')
            ed.wait_for('ticks++')
            ed.keys(*([DOWN] * 4), *DEBUG, 'b')
            ed.wait_for('Added breakpoint at line 5')
            ed.keys(*DEBUG, 'a', delay=0.5)
            ed.wait_for('Attach to')
            ed.keys(*str(program.pid), delay=0.05)
            ed.wait_for(ed.path('t'))
            ed.keys('\r', delay=0.3)
            ed.wait_for('stopped (breakpoint)', 30)
            assert re.match(r'\s*5 ▶', ed.text()[4]), ed.text()[4]
            ed.keys(*DEBUG, 'q', delay=0.5)
            ed.wait_for('Detached from the program', 20)
            ed.pump(0.5)
            assert program.poll() is None, 'the program ended with the session'
            ed.quit()
        finally:
            program.kill()
            program.wait()


def debugger_lldb(binary, repo):
    """The session against lldb-dap, as packages/c sets it up on macOS."""
    if sys.platform != 'darwin' or not shutil.which('cc'):
        raise Skip('macOS only')
    if subprocess.run(['xcrun', '-f', 'lldb-dap'], capture_output=True).returncode != 0:
        raise Skip('no lldb-dap (Xcode command line tools)')
    debug_session(binary, repo, '(define LSP #f)\n(add-package "c")')


def debugger_python_in_terminal(binary, repo):
    """debugpy with "console": "integratedTerminal": its runInTerminal
    request runs the program in a *debuggee* pane, where it reads what is
    typed, while breakpoints and the panel work as usual."""
    if subprocess.run([sys.executable, '-c', 'import debugpy'],
                      capture_output=True).returncode != 0:
        raise Skip('no debugpy (pip install debugpy)')
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        with open(ed.path('t.py'), 'w') as f:
            f.write("name = input('name? ')\n"
                    "total = len(name)\n"
                    "print('hello', name, total)\n")
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(dap-adapter "py" "%s" "-m" "debugpy.adapter")\n'
                    '(define (debug-in-terminal) (debug-start \'(("program" . "%s")'
                    ' ("console" . "integratedTerminal"))))\n'
                    % (sys.executable, ed.path('t.py')))
        ed.start('t.py')
        ed.wait_for('hello')
        ed.keys(DOWN, *DEBUG, 'b')
        ed.keys('\x1bx', delay=0.4)
        ed.keys(*'debug-in-terminal', delay=0.02)
        ed.keys('\r', delay=0.3)
        ed.wait_for('*debuggee*', 30)
        for _ in range(100):  # the program's prompt, not the code's line
            if any(line.startswith('name?') for line in ed.text()):
                break
            ed.pump(0.1)
        ed.write('bob\r')                # the program's pane has the keys
        ed.wait_for('stopped (breakpoint)', 20)
        ed.keys('\x1b', delay=0.3)       # out of the terminal's keys
        ed.keys(*DEBUG, 'c')
        ed.wait_for('hello bob 3', 20)
        ed.wait_for('exited with code 0', 20)
        ed.quit()


def debugger_logpoints(binary, repo):
    """A logpoint writes to the output without stopping; a breakpoint with
    a hit count lets the first hits go by."""
    if subprocess.run([sys.executable, '-c', 'import debugpy'],
                      capture_output=True).returncode != 0:
        raise Skip('no debugpy (pip install debugpy)')
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        with open(ed.path('t.py'), 'w') as f:
            f.write("total = 0\n"
                    "for i in range(5):\n"
                    "    total += i\n"
                    "    total += 0\n"
                    "print('total', total)\n")
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(dap-adapter "py" "%s" "-m" "debugpy.adapter")\n'
                    '(define (debug-t) (debug-start \'(("program" . "%s"))))\n'
                    % (sys.executable, ed.path('t.py')))
        ed.start('t.py')
        ed.wait_for('total')
        ed.keys(DOWN, DOWN, *DEBUG, 'L', delay=0.3)
        ed.wait_for('Log (')
        ed.write('i is {i}\r')
        ed.wait_for('Line 3 logs: i is {i}')
        assert re.match(r'\s*3 ◉', ed.text()[2]), ed.text()[2]
        ed.keys(DOWN, *DEBUG, 'h', delay=0.3)
        ed.wait_for('Stop after how many hits')
        ed.write('3\r')
        ed.wait_for('Breakpoint at line 4 stops on hit 3')
        assert re.match(r'\s*4 ◆', ed.text()[3]), ed.text()[3]
        ed.keys('\x1bx', delay=0.4)
        ed.keys(*'debug-t', delay=0.02)
        ed.keys('\r', delay=0.3)
        ed.wait_for('stopped (breakpoint)', 30)
        ed.wait_for('i = 2')
        ed.wait_for('i is 2')
        assert not any('i is 3' in line for line in ed.text())
        ed.keys(*DEBUG, 'b')  # line 4: off again, so it runs to the end
        ed.keys(*DEBUG, 'c')
        ed.wait_for('exited with code 0', 20)
        ed.keys(*DEBUG, 'o', delay=0.5)
        ed.wait_for('i is 4')
        ed.wait_for('total 10')
        ed.quit()


def debugger_exceptions(binary, repo):
    """Exception breakpoints: with debugpy's "raised" filter on, an
    exception the program catches still stops it; the prompt lists the
    adapter's filters once a session runs."""
    if subprocess.run([sys.executable, '-c', 'import debugpy'],
                      capture_output=True).returncode != 0:
        raise Skip('no debugpy (pip install debugpy)')
    with Beluga(binary, repo, rows=30, cols=140) as ed:
        with open(ed.path('t.py'), 'w') as f:
            f.write("try:\n"
                    "    raise ValueError('boom')\n"
                    "except ValueError:\n"
                    "    pass\n"
                    "print('done')\n")
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(dap-adapter "py" "%s" "-m" "debugpy.adapter")\n'
                    '(define (debug-t) (debug-start \'(("program" . "%s"))))\n'
                    % (sys.executable, ed.path('t.py')))
        ed.start('t.py')
        ed.wait_for('boom')
        ed.keys(*DEBUG, 'E', delay=0.3)
        ed.wait_for('Stop on exceptions')
        ed.write('raised\r')
        ed.wait_for('Exceptions that stop the program: raised')
        ed.keys('\x1bx', delay=0.4)
        ed.keys(*'debug-t', delay=0.02)
        ed.keys('\r', delay=0.3)
        ed.wait_for('stopped (', 30)
        for _ in range(50):  # the call stack comes after the stop
            if re.match(r'\s*2 ▶', ed.text()[1]):
                break
            ed.pump(0.1)
        assert re.match(r'\s*2 ▶', ed.text()[1]), ed.text()[1]
        ed.keys(*DEBUG, 'E', delay=0.3)
        ed.wait_for('*raised (')
        ed.wait_for('uncaught (')
        ed.keys('\x1b', delay=0.3)
        ed.keys(*DEBUG, 'c')
        ed.wait_for('exited with code 0', 20)
        ed.quit()


def debugger_watches(binary, repo):
    """Watch expressions: shown in the panel with their value at each stop,
    added before the session or while it's paused, and taken away. The
    lines up to the stopped one show their variables' values."""
    if subprocess.run([sys.executable, '-c', 'import debugpy'],
                      capture_output=True).returncode != 0:
        raise Skip('no debugpy (pip install debugpy)')
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        with open(ed.path('t.py'), 'w') as f:
            f.write("total = 0\n"
                    "for i in range(3):\n"
                    "    total += i\n"
                    "print('total', total)\n")
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(dap-adapter "py" "%s" "-m" "debugpy.adapter")\n'
                    '(define (debug-t) (debug-start \'(("program" . "%s"))))\n'
                    % (sys.executable, ed.path('t.py')))
        ed.start('t.py')
        ed.wait_for('total')
        ed.keys(*DEBUG, 'w', delay=0.3)
        ed.write('total * 10\r')
        ed.wait_for('Watching total * 10')
        ed.keys(DOWN, DOWN, *DEBUG, 'b')
        ed.keys('\x1bx', delay=0.4)
        ed.keys(*'debug-t', delay=0.02)
        ed.keys('\r', delay=0.3)
        ed.wait_for('stopped (breakpoint)', 30)
        ed.wait_for('total * 10 = 0')
        ed.keys(*DEBUG, 'w', delay=0.3)
        ed.write('i + 100\r')
        ed.wait_for('i + 100 = 100')
        ed.keys(*DEBUG, 'c')
        ed.wait_for('i + 100 = 101', 20)
        ed.keys(*DEBUG, 'c')
        ed.wait_for('i + 100 = 102', 20)
        ed.wait_for('total * 10 = 10')
        # The values after the lines that use them (DEBUG-INLINE-VALUES).
        ed.wait_for('total = 1, i = 2')
        assert 'total = 1, i = 2' in ed.text()[2], ed.text()[2]
        assert 'i = 2' in ed.text()[1], ed.text()[1]
        assert '=' not in ed.text()[3], ed.text()[3]
        ed.keys(*DEBUG, 'W', delay=0.3)   # offers the last one added
        ed.wait_for('Stop watching: i + 100')
        ed.write('\r')
        ed.wait_for('Stopped watching i + 100')
        assert not any('i + 100' in line for line in ed.text()[:-1])
        assert any('total * 10 = 10' in line for line in ed.text())
        ed.keys(*DEBUG, 'b')  # off again, so it runs to the end
        ed.keys(*DEBUG, 'c')
        ed.wait_for('exited with code 0', 20)
        ed.quit()


def python_session(ed, adapter_lisp):
    """A breakpoint, a stop, a variable, then the end, with debugpy reached
    as `adapter_lisp` (the arguments of dap-adapter after "py")."""
    with open(ed.path('t.py'), 'w') as f:
        f.write("total = 0\n"
                "for i in range(3):\n"
                "    total += i\n"
                "print('total', total)\n")
    with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
        f.write('\n(dap-adapter "py" %s)\n'
                '(define (debug-t) (debug-start \'(("program" . "%s"))))\n'
                % (adapter_lisp, ed.path('t.py')))
    ed.start('t.py')
    ed.wait_for('total')
    ed.keys(DOWN, DOWN, *DEBUG, 'b')
    ed.keys('\x1bx', delay=0.4)
    ed.keys(*'debug-t', delay=0.02)
    ed.keys('\r', delay=0.3)
    ed.wait_for('stopped (breakpoint)', 30)
    ed.wait_for('total = 0')
    ed.keys(*DEBUG, 'b')  # off again, so it runs to the end
    ed.keys(*DEBUG, 'c')
    ed.wait_for('exited with code 0', 20)
    ed.keys(*DEBUG, 'o', delay=0.5)
    ed.wait_for('total 3')
    ed.quit()


def debugger_over_tcp(binary, repo):
    """An adapter that listens on a port: started by beluga with {port}
    in its arguments, or already running and named as tcp:HOST:PORT."""
    if subprocess.run([sys.executable, '-c', 'import debugpy'],
                      capture_output=True).returncode != 0:
        raise Skip('no debugpy (pip install debugpy)')
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        python_session(ed, '"%s" "-m" "debugpy.adapter" "--host" "127.0.0.1" "--port" "{port}"'
                       % sys.executable)
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        adapter = subprocess.Popen([sys.executable, '-m', 'debugpy.adapter', '--host',
                                    '127.0.0.1', '--port', str(port)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            python_session(ed, '"tcp:127.0.0.1:%d"' % port)
        finally:
            adapter.kill()
            adapter.wait()


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


def daemon_prompt_other_windows(binary, repo):
    """While one window has a prompt open, another goes on: it takes keys
    and redraws, but can't kill or edit the buffer the prompt's command
    holds. A second prompt opened in it closes first; then the first
    window's keys are read again."""
    name = daemon_name('prompt')
    with Beluga(binary, repo) as a:
        try:
            with open(a.path('a.txt'), 'w') as f:
                f.write('alpha\n')
            with open(a.path('b.txt'), 'w') as f:
                f.write('beta\n')
            rc, out = a.run('--daemon=' + name)
            assert rc == 0, out
            a.start('-c', '--name=' + name, 'a.txt')
            a.wait_for('alpha')
            with Beluga(binary, repo, home=a.home) as b:
                b.start('-c', '--name=' + name, 'b.txt')
                b.wait_for('beta')
                a.keys(CTRL_X, 'f', delay=0.5)       # A: the open-file prompt
                a.wait_for('Open :')
                b.keys(*'xyz', delay=0.1)            # B goes on
                b.wait_for('xyzbeta')
                a.pump(0.3)
                # A's buffer is held by its prompt: B can't kill or edit it.
                for _ in range(3):                   # to a.txt
                    if any('alpha' in line for line in b.text()[:2]):
                        break
                    b.keys(CTRL_X, '\x1b[C', delay=0.4)
                b.wait_for('alpha')
                b.keys(CTRL_X, 'k', delay=0.4)
                b.wait_for('Another window has a prompt open on this buffer')
                b.keys('Q', delay=0.3)
                assert not any('Qalpha' in line for line in b.text()), b.text()[:3]
                for _ in range(3):                   # back to b.txt
                    if any('xyzbeta' in line for line in b.text()):
                        break
                    b.keys(CTRL_X, '\x1b[C', delay=0.4)
                b.wait_for('xyzbeta')
                a.pump(0.3)
                b.keys('\x1bx', delay=0.4)          # B: a prompt of its own
                b.wait_for('M-x:')
                a.keys('\x1b', delay=0.5)           # read once B's is closed
                assert not any('Aborted' in line for line in a.text()), a.text()[-2:]
                b.keys('\x1b', delay=0.4)
                b.wait_for('Aborted')
                a.wait_for('Aborted', 5)
                for _ in range(3):
                    b.keys(CTRL_X, 'u', delay=0.2)
                b.quit()
            a.quit()
        finally:
            a.run('--kill', '--force', '--name=' + name)

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


def daemon_debug_per_project(binary, repo):
    """In a daemon, each project has its own debug session: one paused in
    project a doesn't stop project b from debugging, and a window opened on
    a later finds a's session where it was left."""
    if subprocess.run([sys.executable, '-c', 'import debugpy'],
                      capture_output=True).returncode != 0:
        raise Skip('no debugpy (pip install debugpy)')
    name = daemon_name('dbg')
    with Beluga(binary, repo, rows=30, cols=120) as ed:
        for proj in ('a', 'b'):
            os.makedirs(ed.path(proj + '/.git'))
            with open(ed.path(proj + '/t.py'), 'w') as f:
                f.write("total = 0\n"
                        "for i in range(3):\n"
                        "    total += i\n"
                        "print('%s total', total)\n" % proj)
        with open(ed.path('.beluga/packages/personal-config/init.lisp'), 'a') as f:
            f.write('\n(dap-adapter "py" "%s" "-m" "debugpy.adapter")\n' % sys.executable)
            for proj in ('a', 'b'):
                f.write('(define (debug-%s) (debug-start \'(("program" . "%s"))))\n'
                        % (proj, ed.path(proj + '/t.py')))
        try:
            rc, out = ed.run('--daemon=' + name)
            assert rc == 0, out
            for proj in ('a', 'b'):
                ed.start('-c', '--name=' + name, 't.py', cwd=proj)
                ed.wait_for('%s total' % proj)
                ed.keys(DOWN, DOWN, *DEBUG, 'b')
                ed.keys('\x1bx', delay=0.4)
                ed.keys(*('debug-' + proj), delay=0.02)
                ed.keys('\r', delay=0.3)
                ed.wait_for('stopped (breakpoint)', 30)
                ed.quit()
            ed.start('-c', '--name=' + name, 't.py', cwd='a')
            ed.wait_for('a total')
            ed.keys(DOWN, DOWN, *DEBUG, 'b')  # off again, so it runs to the end
            ed.keys(*DEBUG, 'c')
            ed.wait_for('exited with code 0', 20)
            ed.keys(*DEBUG, 'o', delay=0.5)
            ed.wait_for('a total 3')
            assert not any('b total 3' in line for line in ed.text())
            ed.quit()
        finally:
            ed.run('--kill', '--force', '--name=' + name)


def project_root_start(binary, repo):
    """With a project open, the open-file prompt, dired's prompt and a new
    terminal start at the project root, not where beluga was started.
    Wide, so a long $TMPDIR (macOS) fits on the prompt line."""
    with Beluga(binary, repo, cols=250) as ed:
        os.makedirs(ed.path('proj/.git'))
        os.makedirs(ed.path('proj/sub'))
        os.makedirs(ed.path('elsewhere'))
        with open(ed.path('proj/sub/a.txt'), 'w') as f:
            f.write('x\n')
        ed.start('../proj/sub/a.txt', cwd='elsewhere')
        ed.keys(CTRL_X, 'f', delay=0.4)
        ed.wait_for('Open :')
        prompt = [line for line in ed.text() if line.startswith('Open :')]
        assert prompt and prompt[0].endswith('/proj/'), prompt
        ed.keys('\x1b', delay=0.3)
        ed.keys(CTRL_X, 'd', delay=0.4)
        ed.wait_for('Dired (directory):')
        prompt = [line for line in ed.text() if line.startswith('Dired')]
        assert prompt and prompt[0].rstrip('/').endswith('/proj'), prompt
        ed.keys('\x1b', delay=0.3)
        open_terminal(ed)
        shell(ed, 'pwd')
        ed.wait_for('/proj', 5)
        assert not any('elsewhere' in line for line in ed.text()), ed.text()
        ed.quit()


TESTS = [
    ('breakpoint-gutter', breakpoint_gutter),
    ('pane-cursors', pane_cursors),
    ('mouse-wheel-and-click', mouse_wheel_and_click),
    ('mouse-split-panes', mouse_split_panes),
    ('daemon-mouse', daemon_mouse),
    ('debugger-gdb', debugger_gdb),
    ('debugger-attach', debugger_attach),
    ('debugger-lldb', debugger_lldb),
    ('debugger-python-in-terminal', debugger_python_in_terminal),
    ('debugger-over-tcp', debugger_over_tcp),
    ('debugger-logpoints', debugger_logpoints),
    ('debugger-exceptions', debugger_exceptions),
    ('debugger-watches', debugger_watches),
    ('terminal-app-shift-function-keys', terminal_app_shift_function_keys),
    ('tab-completion-list', tab_completion_list),
    ('project-root-start', project_root_start),
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
    ('daemon-prompt-other-windows', daemon_prompt_other_windows),
    ('daemon-debug-per-project', daemon_debug_per_project),
    ('terminal-long-burst', terminal_long_burst),
]


def main():
    """pty_tests.py --list prints the tests' names (meson.build makes one
    test of each); pty_tests.py BINARY REPO [NAME...] runs the named tests,
    or all of them."""
    if sys.argv[1:] == ['--list']:
        print('\n'.join(name for name, _ in TESTS))
        return 0
    if pty_harness.pyte is None:
        print('pty tests skipped: pyte is not installed (python3-pyte)')
        return 77
    binary, repo, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    known = dict(TESTS)
    unknown = [name for name in names if name not in known]
    if unknown:
        print('no such test: %s' % ', '.join(unknown))
        return 2
    tests = [(name, known[name]) for name in names] if names else TESTS
    return run_tests(tests, binary, repo)


if __name__ == '__main__':
    sys.exit(main())
