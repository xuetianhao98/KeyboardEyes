#!/usr/bin/env python3
"""Exercise the real viewer process against isolated SQLite databases."""

import argparse
import fcntl
import os
from pathlib import Path
import queue
import signal
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest


VIEWER = None
SCHEMA = """
CREATE TABLE key_counts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    key_name TEXT NOT NULL UNIQUE,
    press_count INTEGER NOT NULL DEFAULT 0
        CHECK (typeof(press_count) = 'integer' AND press_count >= 0)
);
"""


class RunningViewer:
    def __init__(self, arguments, cwd=None):
        self.process = subprocess.Popen(
            [VIEWER] + arguments, cwd=cwd, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.stdout = queue.Queue()
        self.stderr = queue.Queue()
        self.threads = []
        for stream, destination in ((self.process.stdout, self.stdout),
                                    (self.process.stderr, self.stderr)):
            thread = threading.Thread(target=self._read,
                                      args=(stream, destination), daemon=True)
            thread.start()
            self.threads.append(thread)

    @staticmethod
    def _read(stream, destination):
        try:
            for line in stream:
                destination.put(line)
        finally:
            destination.put(None)

    @staticmethod
    def until(destination, predicate, timeout):
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("Timed out waiting for output: " + repr(lines))
            try:
                line = destination.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError("Timed out waiting for output: " + repr(lines))
            if line is None:
                raise AssertionError("Process closed output early: " + repr(lines))
            lines.append(line)
            if predicate(line):
                return "".join(lines)

    def snapshot(self, timeout=4):
        return self.until(self.stdout, lambda line: line.startswith("Rows: "),
                          timeout)

    def log(self, substring, timeout=4):
        return self.until(self.stderr, lambda line: substring in line, timeout)

    def stop(self, exit_signal=signal.SIGTERM):
        self.process.send_signal(exit_signal)
        return self.process.wait(timeout=2)

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        for thread in self.threads:
            thread.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()


def table_rows(snapshot):
    lines = [line for line in snapshot.splitlines() if line.startswith("|")]
    return [[cell.strip() for cell in line.split("|")[1:-1]] for line in lines]


class ViewerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="keyboardeyes-viewer-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.database = self.directory / "stats.db"

    def create_database(self, rows=(), schema=SCHEMA):
        connection = sqlite3.connect(str(self.database))
        try:
            connection.executescript(schema)
            if rows:
                connection.executemany(
                    "INSERT INTO key_counts (id, key_name, press_count) VALUES (?, ?, ?)",
                    rows)
            connection.commit()
        finally:
            connection.close()

    def run_viewer(self, *arguments, **kwargs):
        return subprocess.run([VIEWER] + list(arguments), text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=4, **kwargs)

    def test_complete_sorted_readonly_snapshot(self):
        self.create_database([(7, "KEY_ENTER", 9223372036854775807),
                              (2, "KEY_A", 0)])
        original = self.database.read_bytes()
        with RunningViewer(["--db", str(self.database), "--interval", "60"]) as app:
            snapshot = app.snapshot(timeout=2)
            self.assertEqual(table_rows(snapshot), [
                ["id", "key_name", "press_count"],
                ["2", "KEY_A", "0"],
                ["7", "KEY_ENTER", "9223372036854775807"]])
            self.assertRegex(snapshot, r"Table: key_counts \| Read at: .*Z")
            self.assertIn("Rows: 2\n", snapshot)
            self.assertEqual(app.stop(), 0)
        self.assertEqual(self.database.read_bytes(), original)
        self.assertEqual(sorted(path.name for path in self.directory.iterdir()),
                         ["stats.db"])

    def test_periodic_refresh_and_unchanged_snapshot(self):
        self.create_database([(1, "KEY_A", 2)])
        with RunningViewer(["--db", str(self.database), "--interval", "1"]) as app:
            first = app.snapshot()
            started = time.monotonic()
            second = app.snapshot()
            self.assertGreater(time.monotonic() - started, 0.7)
            self.assertEqual(table_rows(first), table_rows(second))
            connection = sqlite3.connect(str(self.database), timeout=0)
            try:
                # This commit also verifies that waiting does not hold a read lock.
                connection.execute("UPDATE key_counts SET press_count = 9")
                connection.execute(
                    "INSERT INTO key_counts (key_name, press_count) VALUES ('KEY_B', 3)")
                connection.commit()
            finally:
                connection.close()
            self.assertEqual(table_rows(app.snapshot())[1:],
                             [["1", "KEY_A", "9"], ["2", "KEY_B", "3"]])
            self.assertEqual(app.stop(), 0)

    def test_default_path_and_empty_table(self):
        self.create_database()
        with RunningViewer([], cwd=self.directory) as app:
            self.assertIn("interval: 5 seconds", app.log("Started viewer"))
            snapshot = app.snapshot(timeout=2)
            self.assertEqual(table_rows(snapshot), [["id", "key_name", "press_count"]])
            self.assertIn("Rows: 0", snapshot)
            self.assertEqual(app.stop(), 0)

    def test_control_characters_are_escaped_without_truncation(self):
        self.create_database([(1, "KEY_\n\x00\x1b|\\END", 3)])
        with RunningViewer(["--db", str(self.database)]) as app:
            self.assertEqual(table_rows(app.snapshot())[1],
                             ["1", r"KEY_\x0A\x00\x1B\x7C\\END", "3"])
            self.assertEqual(app.stop(), 0)

    def test_help_and_invalid_arguments(self):
        result = self.run_viewer("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--interval 5", result.stdout)
        for arguments in [
                ["--unknown"], ["positional"], ["--db"], ["--db", ""],
                ["--db", "a", "--db", "b"], ["--interval"],
                ["--interval", "1", "--interval", "2"],
                *[["--interval", value] for value in
                  ("0", "-1", "1.5", "abc", "1x", "+1", " 1", "2147484",
                   "999999999999999999999999")]]:
            with self.subTest(arguments=arguments):
                result = self.run_viewer(*arguments)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertIn("Usage:", result.stderr)

    def test_missing_file_and_disallowed_paths(self):
        result = self.run_viewer("--db", str(self.database))
        self.assertEqual(result.returncode, 1)
        self.assertIn(str(self.database), result.stderr)
        self.assertFalse(self.database.exists())
        for path in (":memory:", "file:" + str(self.database), str(self.directory)):
            with self.subTest(path=path):
                result = self.run_viewer("--db", path)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
        self.assertFalse(self.database.exists())

    def test_bad_database_and_schema(self):
        cases = [None, "CREATE TABLE unrelated (value INTEGER);",
                 "CREATE TABLE key_counts (id INTEGER, key_name TEXT);"]
        for schema in cases:
            with self.subTest(schema=schema):
                if self.database.exists():
                    self.database.unlink()
                if schema is None:
                    self.database.write_bytes(b"This is not a SQLite database.\n" * 20)
                else:
                    self.create_database(schema=schema)
                result = self.run_viewer("--db", str(self.database))
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertIn(str(self.database), result.stderr)

    def test_invalid_rows_never_print_partial_snapshot(self):
        schema = "CREATE TABLE key_counts (id, key_name, press_count);"
        for invalid in [(2, "KEY_B", -1), (2, "KEY_B", "invalid"),
                        (2, None, 1), ("bad-id", "KEY_B", 1),
                        (2, "KEY_B", 1.5)]:
            with self.subTest(invalid=invalid):
                if self.database.exists():
                    self.database.unlink()
                self.create_database([(1, "KEY_A", 1), invalid], schema=schema)
                result = self.run_viewer("--db", str(self.database))
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertIn(str(self.database), result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "root bypasses file permission checks")
    def test_permission_denied(self):
        self.create_database()
        self.database.chmod(0)
        try:
            result = self.run_viewer("--db", str(self.database))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, "")
            self.assertIn(str(self.database), result.stderr)
        finally:
            self.database.chmod(0o600)

    def test_lock_contention_and_recovery(self):
        self.create_database([(1, "KEY_A", 2)])
        connection = sqlite3.connect(str(self.database), timeout=0)
        try:
            connection.execute("BEGIN EXCLUSIVE")
            connection.execute("UPDATE key_counts SET press_count = 8")
            with RunningViewer(["--db", str(self.database), "--interval", "1"]) as app:
                self.assertIn("locked", app.log("retrying next interval"))
                self.assertTrue(app.stdout.empty())
                connection.commit()
                self.assertEqual(table_rows(app.snapshot())[1], ["1", "KEY_A", "8"])
                self.assertEqual(app.stop(), 0)
        finally:
            connection.close()

    def test_collector_flock_does_not_block_reading(self):
        self.create_database([(1, "KEY_A", 1)])
        with self.database.open("rb") as locked:
            fcntl.flock(locked.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            with RunningViewer(["--db", str(self.database)]) as app:
                self.assertIn("Rows: 1", app.snapshot(timeout=2))
                self.assertEqual(app.stop(), 0)

    def test_signals_interrupt_long_interval(self):
        self.create_database()
        for exit_signal in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(exit_signal=exit_signal):
                with RunningViewer(["--db", str(self.database),
                                    "--interval", "2147483"]) as app:
                    app.snapshot(timeout=2)
                    started = time.monotonic()
                    self.assertEqual(app.stop(exit_signal), 0)
                    self.assertLess(time.monotonic() - started, 1)

    def test_stdout_failure(self):
        self.create_database()
        with open("/dev/full", "w") as output:
            result = subprocess.run([VIEWER, "--db", str(self.database)],
                                    stdout=output, stderr=subprocess.PIPE,
                                    text=True, timeout=4)
        self.assertEqual(result.returncode, 1)
        self.assertIn("stdout failed", result.stderr)

    def test_broken_pipe_reports_error(self):
        self.create_database()
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            result = subprocess.run([VIEWER, "--db", str(self.database)],
                                    stdout=write_fd, stderr=subprocess.PIPE,
                                    text=True, timeout=4)
        finally:
            os.close(write_fd)
        self.assertEqual(result.returncode, 1)
        self.assertIn("stdout failed", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--viewer", required=True)
    arguments, remaining = parser.parse_known_args()
    VIEWER = str(Path(arguments.viewer).resolve())
    unittest.main(argv=[__file__] + remaining)
