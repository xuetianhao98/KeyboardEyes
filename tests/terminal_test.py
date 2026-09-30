#!/usr/bin/env python3
"""Test the terminal UI using real processes and disposable pseudo-terminals."""

import argparse
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import sqlite3
import struct
import subprocess
import tempfile
import termios
import time
import unittest

VIEWER = None
ENTER = b"\x1b[?1049h"
LEAVE = b"\x1b[?1049l"
SHOW_CURSOR = b"\x1b[?25h"
END_FRAME = b"\x1b[J"


class TerminalViewer:
    def __init__(self, arguments, columns=140, rows=33, term="xterm-256color",
                 color_term=""):
        self.master, self.slave = pty.openpty()
        self.original_termios = termios.tcgetattr(self.slave)
        self.set_size(columns, rows)
        environment = dict(os.environ, TERM=term, COLORTERM=color_term)
        self.process = subprocess.Popen(
            [VIEWER] + arguments, stdin=subprocess.DEVNULL, stdout=self.slave,
            stderr=subprocess.PIPE, env=environment)
        self.output = b""
        self.pending = b""
        self.errors = b""

    def set_size(self, columns, rows):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, columns, 0, 0))

    def resize(self, columns, rows):
        self.set_size(columns, rows)
        self.process.send_signal(signal.SIGWINCH)

    def pump(self, timeout):
        descriptors = [self.master]
        if not self.process.stderr.closed:
            descriptors.append(self.process.stderr.fileno())
        readable, _, _ = select.select(descriptors, [], [], timeout)
        for descriptor in readable:
            try:
                data = os.read(descriptor, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            if descriptor == self.master:
                self.output += data
                self.pending += data
            else:
                self.errors += data
                if not data:
                    self.process.stderr.close()

    def until(self, predicate, timeout=4):
        deadline = time.monotonic() + timeout
        while not predicate():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("Timed out; stderr=" + repr(self.errors) +
                                     "; output tail=" + repr(self.output[-500:]))
            self.pump(min(remaining, 0.1))

    def frame(self, timeout=4):
        self.until(lambda: END_FRAME in self.pending, timeout)
        frame, self.pending = self.pending.split(END_FRAME, 1)
        return frame + END_FRAME

    def finish(self, exit_signal=None):
        if exit_signal is not None:
            self.process.send_signal(exit_signal)
        self.until(lambda: self.process.poll() is not None)
        self.pump(0.05)
        return self.process.returncode

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.finish()
            except AssertionError:
                self.process.kill()
                self.process.wait(timeout=2)
        self.process.stderr.close()
        os.close(self.slave)
        os.close(self.master)


class TerminalTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="keyboardeyes-terminal-")
        self.addCleanup(self.temporary.cleanup)
        self.database = Path(self.temporary.name) / "stats.db"
        self.connection = sqlite3.connect(str(self.database), timeout=0)
        self.addCleanup(self.connection.close)
        self.connection.executescript("""
            CREATE TABLE key_counts (id INTEGER PRIMARY KEY, key_name TEXT UNIQUE,
                                     press_count INTEGER);
            INSERT INTO key_counts VALUES (1, 'KEY_A', 12), (2, 'KEY_SPACE', 100);
        """)
        self.arguments = ["--db", str(self.database), "--interval", "1"]

    def assert_restored(self, app):
        self.assertIn(LEAVE, app.output)
        self.assertIn(SHOW_CURSOR, app.output)
        self.assertEqual(termios.tcgetattr(app.slave), app.original_termios)

    def test_auto_heatmap_updates_and_restores(self):
        with TerminalViewer(self.arguments) as app:
            first = app.frame()
            self.assertIn(ENTER, first)
            self.assertIn(b"KEY_A=12", first)
            self.assertIn(b"48;5;196m", first)
            self.connection.execute("UPDATE key_counts SET press_count=999 WHERE key_name='KEY_A'")
            self.connection.commit()
            self.assertIn(b"KEY_A=999", app.frame())
            self.assertEqual(app.finish(signal.SIGINT), 0)
            self.assert_restored(app)

    def test_resize_reuses_snapshot_and_keeps_poll_deadline(self):
        arguments = ["--db", str(self.database), "--interval", "2"]
        with TerminalViewer(arguments) as app:
            first = app.frame()
            stamp = re.search(rb"Last read: ([^\r\n\x1b]+)", first).group(1)
            self.connection.execute("UPDATE key_counts SET press_count=77 WHERE key_name='KEY_A'")
            self.connection.commit()
            started = time.monotonic()
            app.resize(80, 24)
            small = app.frame(timeout=1)
            self.assertIn(b"Terminal too small: need 140x33, current 80x24", small)
            self.assertIn(stamp, small)
            self.assertNotIn(b"+---+", small)
            # Keep resizing past the original deadline. A resize must not defer
            # the scheduled database read, even when the keyboard cannot fit.
            while time.monotonic() - started < 2.3:
                app.resize(81, 24)
                app.frame(timeout=1)
                time.sleep(0.1)
            app.resize(140, 33)
            while True:
                restored = app.frame(timeout=1)
                if b"ANSI 104" in restored:
                    break
            self.assertIn(b"KEY_A=77", restored)
            self.assertEqual(app.finish(signal.SIGTERM), 0)
            self.assert_restored(app)

    def test_resize_does_not_query_again(self):
        with TerminalViewer(["--db", str(self.database), "--interval", "60"]) as app:
            app.frame()
            self.connection.execute("UPDATE key_counts SET press_count=500 WHERE key_name='KEY_A'")
            self.connection.commit()
            app.resize(160, 40)
            frame = app.frame(timeout=1)
            self.assertIn(b"KEY_A=12", frame)
            self.assertNotIn(b"KEY_A=500", frame)
            self.assertEqual(app.finish(signal.SIGTERM), 0)

    def test_busy_keeps_last_snapshot_and_recovers(self):
        with TerminalViewer(self.arguments) as app:
            first = app.frame()
            stamp = re.search(rb"Last read: ([^\r\n\x1b]+)", first).group(1)
            self.connection.execute("BEGIN EXCLUSIVE")
            self.connection.execute("UPDATE key_counts SET press_count=987 WHERE key_name='KEY_A'")
            busy = app.frame()
            self.assertIn(b"KEY_A=12", busy)
            self.assertIn(stamp, busy)
            self.assertIn(b"retrying next interval", busy)
            self.assertNotIn(b"retrying", app.errors)
            self.connection.commit()
            recovered = app.frame()
            self.assertIn(b"KEY_A=987", recovered)
            self.assertIn(b"Read successful", recovered)
            self.assertEqual(app.finish(signal.SIGTERM), 0)

    def test_first_read_busy(self):
        self.connection.execute("BEGIN EXCLUSIVE")
        with TerminalViewer(self.arguments) as app:
            frame = app.frame()
            self.assertIn(b"Waiting for first successful read", frame)
            self.assertIn(b"retrying next interval", frame)
            self.connection.commit()
            self.assertIn(b"KEY_A=12", app.frame())
            self.assertEqual(app.finish(signal.SIGTERM), 0)

    def test_fatal_read_error_restores_terminal(self):
        with TerminalViewer(self.arguments) as app:
            app.frame()
            self.connection.execute("DROP TABLE key_counts")
            self.connection.commit()
            self.assertEqual(app.finish(), 1)
            self.assert_restored(app)
            self.assertIn(b"no such table", app.errors)

    def test_initial_error_restores_terminal(self):
        self.connection.execute("DROP TABLE key_counts")
        self.connection.commit()
        with TerminalViewer(self.arguments) as app:
            self.assertEqual(app.finish(), 1)
            self.assert_restored(app)

    def test_fallback_and_explicit_table(self):
        for term, color, extra in [("dumb", "", []), ("", "", []),
                                   ("xterm", "", []),
                                   ("xterm-256color", "", ["--view", "table"])]:
            with self.subTest(term=term, extra=extra):
                with TerminalViewer(self.arguments + extra, term=term, color_term=color) as app:
                    app.until(lambda: b"Rows: 2" in app.output)
                    self.assertNotIn(b"\x1b[", app.output)
                    self.assertEqual(app.finish(signal.SIGTERM), 0)

    def test_truecolor_and_explicit_keyboard(self):
        for color, extra in [("truecolor", []), ("24bit", []),
                             ("", ["--view", "keyboard"])]:
            with self.subTest(color=color, extra=extra):
                with TerminalViewer(self.arguments + extra, term="xterm", color_term=color) as app:
                    self.assertIn(b"ANSI 104", app.frame())
                    self.assertEqual(app.finish(signal.SIGTERM), 0)
                    self.assert_restored(app)

    def test_invalid_modes_and_non_tty(self):
        for extra in [["--view"], ["--view", "invalid"],
                      ["--view", "table", "--view", "auto"],
                      ["--view", "keyboard"]]:
            with self.subTest(extra=extra):
                result = subprocess.run([VIEWER] + self.arguments + extra,
                                        capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, b"")
        with TerminalViewer(self.arguments + ["--view", "keyboard"], term="dumb") as app:
            self.assertEqual(app.finish(), 1)
            self.assertNotIn(ENTER, app.output)

    def test_closed_terminal_exits_on_output_failure(self):
        with TerminalViewer(self.arguments) as app:
            app.frame()
            os.close(app.master)
            app.master = os.open(os.devnull, os.O_RDONLY)
            self.assertEqual(app.finish(), 1)
            self.assertIn(b"terminal", app.errors.lower())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--viewer", required=True)
    arguments, remaining = parser.parse_known_args()
    VIEWER = str(Path(arguments.viewer).resolve())
    unittest.main(argv=[__file__] + remaining)
