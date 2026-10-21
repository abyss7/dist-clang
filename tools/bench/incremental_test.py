#!/usr/bin/env python3
"""Tests of incremental.py:

  python3 -m unittest tools/bench/incremental_test.py
"""

import os
import signal
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import incremental  # noqa: E402


class ApplyScenarioTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "a.h")
        with open(self.path, "w") as f:
            f.write("int a;\n")

    def tearDown(self):
        self.dir.cleanup()

    def read(self):
        with open(self.path) as f:
            return f.read()

    def test_edit_comment_is_new_every_time(self):
        # The edited version of a previous run may be in the cache already.
        contents = []
        for _ in range(2):
            restore = incremental.apply_scenario("edit-comment", [self.path])
            contents.append(self.read())
            restore()
            self.assertEqual("int a;\n", self.read())
        self.assertNotEqual(contents[0], contents[1])
        for content in contents:
            self.assertTrue(content.startswith("int a;\n//"), content)


class MissesTest(unittest.TestCase):
    def test_compiled_by_daemon(self):
        # A miss of the direct cache isn't always a lookup of the simple one:
        # e.g. the preprocessing is skipped.
        before = {"LOCAL_TASK_DONE": 5, "SIMPLE_CACHE_MISS": 1}
        after = {"LOCAL_TASK_DONE": 8, "SIMPLE_CACHE_MISS": 1}
        self.assertEqual(3, incremental.misses(before, after))


class SourcesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "a.h")
        with open(self.path, "wb") as f:
            f.write(b"int a;\n")

    def tearDown(self):
        self.dir.cleanup()

    def test_refuse_changed_sources(self):
        # The files, that an interrupted run has left changed, aren't the
        # originals.
        for kind in ("edit-source", "edit-comment"):
            with open(self.path, "wb") as f:
                f.write(b"int a;\n")
            incremental.apply_scenario(kind, [self.path])
            with self.assertRaises(SystemExit):
                incremental.apply_scenario(kind, [self.path])

    def test_write_atomically(self):
        inode = os.stat(self.path).st_ino
        restore = incremental.apply_scenario("edit-source", [self.path])
        self.assertNotEqual(inode, os.stat(self.path).st_ino)
        restore()
        with open(self.path, "rb") as f:
            self.assertEqual(b"int a;\n", f.read())
        self.assertEqual(["a.h"], os.listdir(self.dir.name))


class SignalsTest(unittest.TestCase):
    def test_finally_runs_on_sigterm(self):
        old = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGHUP)}
        try:
            incremental.exit_on_signals()
            restored = False
            with self.assertRaises(SystemExit):
                try:
                    os.kill(os.getpid(), signal.SIGHUP)
                    time.sleep(1)
                finally:
                    restored = True
            self.assertTrue(restored)
        finally:
            for s, handler in old.items():
                signal.signal(s, handler)


if __name__ == "__main__":
    unittest.main()
