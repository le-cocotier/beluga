#!/usr/bin/env python3
"""Headless smoke test: runs beluga in a pseudo-terminal with the repo's
default config, types into a file, saves, quits, and checks the result.

    tools/smoke_test.py BELUGA_BINARY REPO_DIR

Exits non-zero if the editor crashes, hangs, exits with an error (a
sanitizer report makes it do so) or saves the wrong text. POSIX only; it
doesn't need pyte (tools/pty_tests.py has the tests that look at the
screen).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import Beluga, run_tests  # noqa: E402


def smoke(binary, repo):
    with Beluga(binary, repo) as ed:
        with open(ed.path('note.txt'), 'w') as f:
            f.write('first line\n')
        ed.start('note.txt')
        # Type at the start of the file, save (CTRL-x CTRL-s), quit
        # (CTRL-x CTRL-c).
        ed.keys('h', 'i', ' ', '\x18', '\x13', '\x18', '\x03', delay=0.3)
        status = ed.wait_exit()
        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, \
            'beluga exited abnormally (status %d)' % status
        with open(ed.path('note.txt')) as f:
            text = f.read()
        assert text == 'hi first line\n', 'saved text is %r' % text


if __name__ == '__main__':
    sys.exit(run_tests([('smoke', smoke)], sys.argv[1], sys.argv[2]))
