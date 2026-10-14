#!/usr/bin/env python3
"""Headless smoke test: runs beluga in a pseudo-terminal with the repo's
default config, types into a file, saves, quits, and checks the result.

    tools/smoke_test.py BELUGA_BINARY REPO_DIR

Exits non-zero if the editor crashes, hangs, exits with an error (a
sanitizer report makes it do so) or saves the wrong text. POSIX only.
"""
import fcntl
import os
import pty
import select
import shutil
import struct
import sys
import tempfile
import termios
import time


def install_config(repo, home):
    beluga = os.path.join(home, '.beluga')
    os.makedirs(os.path.join(beluga, 'config'))
    shutil.copy(os.path.join(repo, 'config', 'init.lisp'),
                os.path.join(beluga, 'config'))
    shutil.copytree(os.path.join(repo, 'packages'),
                    os.path.join(beluga, 'packages'))
    personal = os.path.join(beluga, 'packages', 'personal-config')
    os.makedirs(personal, exist_ok=True)
    shutil.copy(os.path.join(repo, 'config', 'personal-config.lisp'),
                os.path.join(personal, 'init.lisp'))
    shutil.copytree(os.path.join(repo, 'assets'),
                    os.path.join(beluga, 'assets'))


def drain(fd, seconds):
    """Reads (and discards) output for `seconds`; False once the pty closes."""
    end = time.time() + seconds
    while True:
        left = end - time.time()
        if left <= 0:
            return True
        ready, _, _ = select.select([fd], [], [], left)
        if ready:
            try:
                if not os.read(fd, 65536):
                    return False
            except OSError:
                return False


def main():
    binary, repo = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    home = tempfile.mkdtemp(prefix='beluga-smoke-')
    try:
        install_config(repo, home)
        path = os.path.join(home, 'note.txt')
        with open(path, 'w') as f:
            f.write('first line\n')

        pid, fd = pty.fork()
        if pid == 0:
            os.environ['HOME'] = home
            os.environ['TERM'] = 'xterm-256color'
            os.chdir(home)
            os.execv(binary, [binary, path])
        # A real size: the editor lays itself out from it.
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))

        drain(fd, 2.0)
        # Type at the start of the file, save (CTRL-x CTRL-s), quit
        # (CTRL-x CTRL-c). Keys go one at a time, as a person types them.
        for key in [b'h', b'i', b' ', b'\x18', b'\x13', b'\x18', b'\x03']:
            os.write(fd, key)
            drain(fd, 0.3)

        deadline = time.time() + 20
        status = None
        while time.time() < deadline:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                break
            drain(fd, 0.2)
        else:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            print('smoke test: beluga did not exit after CTRL-x CTRL-c')
            return 1

        if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            print('smoke test: beluga exited abnormally (status %d)' % status)
            return 1
        with open(path) as f:
            text = f.read()
        if text != 'hi first line\n':
            print('smoke test: saved text is %r' % text)
            return 1
        print('smoke test: ok')
        return 0
    finally:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
