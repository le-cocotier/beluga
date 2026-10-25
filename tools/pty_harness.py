"""Runs beluga in a pseudo-terminal for the headless tests (POSIX only).

    from pty_harness import Beluga
    with Beluga(binary, repo) as ed:
        ed.keys('hi', '\\x18', '\\x13')       # type, then CTRL-x CTRL-s
        ed.wait_for('File saved')
        print(ed.text())                     # the screen, as pyte sees it

The terminal is sized before the editor starts (it lays itself out from
the size it reads at startup), HOME is a temporary directory holding the
repo's config and packages, and a SIGALRM watchdog ends a test that hangs
instead of letting it block the test run. With pyte installed
(python3-pyte, or pip install pyte), the output also goes through a
terminal emulator, so tests can check what is on screen.
"""
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import termios
import time

try:
    import pyte
except ImportError:
    pyte = None


class Timeout(Exception):
    pass


def _on_alarm(signum, frame):
    raise Timeout('watchdog: the test took too long')


def install_config(repo, home):
    """The repo's init.lisp, packages, personal config and assets, as
    install.sh puts them in ~/.beluga."""
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


class Beluga:
    def __init__(self, binary, repo, args=(), rows=30, cols=100,
                 timeout=60, env=None, home=None):
        self.binary = os.path.abspath(binary)
        self.repo = os.path.abspath(repo)
        self.args = list(args)
        self.rows, self.cols = rows, cols
        self.timeout = timeout
        self.extra_env = env or {}
        # Another Beluga's HOME, to run a second program next to it (a
        # daemon window); it is left for that one to remove.
        self.shared_home = home
        self.home = None
        self.pid = None
        self.fd = None
        self.status = None
        self.screen = pyte.Screen(cols, rows) if pyte else None
        self.stream = pyte.ByteStream(self.screen) if pyte else None

    # ── Lifecycle ──────────────────────────────────────────────────────

    def __enter__(self):
        if self.shared_home:
            self.home = self.shared_home
            self.previous_alarm = None
            return self
        self.home = tempfile.mkdtemp(prefix='beluga-test-')
        install_config(self.repo, self.home)
        self.previous_alarm = signal.signal(signal.SIGALRM, _on_alarm)
        signal.alarm(self.timeout)
        return self

    def start(self, *args):
        """Starts beluga with `args` (paths relative to HOME)."""
        master, slave = pty.openpty()
        # Sized before the editor exists: it reads the size once at startup.
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack('HHHH', self.rows, self.cols, 0, 0))
        pid = os.fork()
        if pid == 0:
            try:
                os.close(master)
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
                for fd in (0, 1, 2):
                    os.dup2(slave, fd)
                if slave > 2:
                    os.close(slave)
                os.environ.update({'HOME': self.home, 'TERM': 'xterm-256color',
                                   'SHELL': '/bin/sh', 'PS1': '$ '})
                os.environ.update(self.extra_env)
                os.chdir(self.home)
                argv = [self.binary] + self.args + list(args)
                os.execv(self.binary, argv)
            finally:
                os._exit(127)
        os.close(slave)
        self.pid, self.fd = pid, master
        self.pump(1.5)  # init.lisp and the first frame
        return self

    def __exit__(self, kind, value, tb):
        if not self.shared_home:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, self.previous_alarm)
        if self.pid and self.status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except OSError:
                pass
        if self.fd is not None:
            os.close(self.fd)
        if not self.shared_home:
            shutil.rmtree(self.home, ignore_errors=True)
        return False

    def path(self, name):
        return os.path.join(self.home, name)

    def run(self, *args):
        """Runs beluga with `args` to the end, outside the pty, with this
        HOME; (exit code, stdout + stderr)."""
        env = dict(os.environ, HOME=self.home, **self.extra_env)
        result = subprocess.run([self.binary] + list(args), cwd=self.home, env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=20)
        return result.returncode, result.stdout.decode(errors='replace')

    # ── Talking to it ──────────────────────────────────────────────────

    def pump(self, seconds):
        """Reads output for `seconds`; False once the pty has closed."""
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return True
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return False
            if not data:
                return False
            if self.stream:
                self.stream.feed(data)

    def keys(self, *keys, delay=0.15):
        """Sends each key (str or bytes) on its own, as a person types."""
        for key in keys:
            os.write(self.fd, key.encode() if isinstance(key, str) else key)
            self.pump(delay)

    def write(self, data):
        """Sends `data` in one go, as a paste or a fast typist would. The
        editor's output is read while the write waits: macOS's pty input
        queue holds about 1 KB, and a blocking write of more stalled until
        the editor read its keys, while the editor stalled drawing to a
        harness that wasn't reading."""
        data = data.encode() if isinstance(data, str) else data
        flags = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        try:
            while data:
                try:
                    data = data[os.write(self.fd, data):]
                except BlockingIOError:
                    self.pump(0.01)
        finally:
            fcntl.fcntl(self.fd, fcntl.F_SETFL, flags)

    def text(self):
        """The screen's lines (needs pyte)."""
        return [line.rstrip() for line in self.screen.display]

    def wait_for(self, needle, seconds=10):
        """Waits until `needle` is on screen (needs pyte)."""
        end = time.time() + seconds
        while time.time() < end:
            if any(needle in line for line in self.screen.display):
                return True
            self.pump(0.1)
        raise AssertionError('%r never showed up; the screen:\n%s'
                             % (needle, '\n'.join(self.text())))

    def quit(self):
        """Leaves a terminal pane (ESC), quits with CTRL-x CTRL-c (again if
        it asks about modified buffers) and checks that the editor exited
        cleanly: under a sanitizer build, a report or a leak fails here."""
        self.keys('\x1b', delay=0.3)
        for _ in range(3):
            try:
                self.keys('\x18', '\x03', delay=0.3)
            except OSError:
                break  # the pty closed: it exited while we were typing
            end = time.time() + 1  # tearing down takes a moment
            done = 0
            while not done and time.time() < end:
                done, status = os.waitpid(self.pid, os.WNOHANG)
                if not done:
                    time.sleep(0.05)
            if done:
                self.status = status
                break
        status = self.status if self.status is not None else self.wait_exit(10)
        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, \
            'beluga exited abnormally (status %d)' % status

    def wait_exit(self, seconds=20):
        """Waits for the editor to exit; its wait status."""
        end = time.time() + seconds
        while time.time() < end:
            done, status = os.waitpid(self.pid, os.WNOHANG)
            if done:
                self.status = status
                return status
            self.pump(0.1)
        raise AssertionError('beluga did not exit')


def run_tests(tests, binary, repo):
    """Runs (name, function(binary, repo)) pairs; 0 if all passed."""
    failed = 0
    for name, test in tests:
        try:
            test(binary, repo)
            print('ok    %s' % name)
        except (AssertionError, Timeout) as e:
            print('FAIL  %s: %s' % (name, e))
            failed += 1
    return 1 if failed else 0
