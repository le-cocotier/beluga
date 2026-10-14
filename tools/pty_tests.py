#!/usr/bin/env python3
"""Headless tests that look at beluga's screen: the editor runs in a
pseudo-terminal (tools/pty_harness.py) and pyte emulates the terminal.

    tools/pty_tests.py BELUGA_BINARY REPO_DIR

POSIX only. Exits 77 (meson's "skipped") when pyte isn't installed
(python3-pyte, or pip install pyte). A test that needs a program the
machine doesn't have (less, vim) is skipped on its own.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pty_harness  # noqa: E402
from pty_harness import Beluga, run_tests  # noqa: E402

CTRL_X = '\x18'


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


TESTS = [
    ('tab-completion-list', tab_completion_list),
    ('terminal-alternate-screen', terminal_alternate_screen),
    ('terminal-vim', terminal_vim),
    ('terminal-colors', terminal_colors),
    ('terminal-wide-characters', terminal_wide_characters),
    ('terminal-fast-typing', terminal_fast_typing),
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
