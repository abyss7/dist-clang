#!/usr/bin/env python3
"""Tests of crash_test.py:

  python3 -m unittest tools/stress/crash_test_test.py
"""

import os
import signal
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crash_test  # noqa: E402


class IsolateEnvironmentTest(unittest.TestCase):
    def test_own_cache_config_and_runtime(self):
        # The daemon of the test mustn't share the cache, the config and the
        # socket with the daemon of the user.
        user = {"HOME": os.path.expanduser("~"),
                "XDG_RUNTIME_DIR": "/run/user/1000",
                "XDG_CACHE_HOME": "/home/user/.cache",
                "XDG_CONFIG_HOME": "/home/user/.config"}
        with mock.patch.dict(os.environ, user, clear=True):
            root = crash_test.isolate_environment()
            try:
                for name in ("XDG_RUNTIME_DIR", "XDG_CACHE_HOME",
                             "XDG_CONFIG_HOME"):
                    self.assertTrue(os.environ[name].startswith(root + "/"),
                                    name)
                    self.assertTrue(os.path.isdir(os.environ[name]), name)
                self.assertEqual(0o700,
                                 os.stat(os.environ["XDG_RUNTIME_DIR"]).st_mode
                                 & 0o777)
                # The unix sockets have the short limit of the path.
                self.assertLess(len(os.environ["XDG_RUNTIME_DIR"]), 64)
            finally:
                crash_test.remove_environment(root)
            self.assertFalse(os.path.exists(root))


class SourcesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "a.cc")
        with open(self.path, "wb") as f:
            f.write(b"int a;\n")

    def tearDown(self):
        self.dir.cleanup()

    def test_refuse_changed_sources(self):
        # The sources, that an interrupted run has left changed, aren't the
        # originals.
        self.assertEqual({self.path: b"int a;\n"},
                         crash_test.read_originals([self.path]))
        with open(self.path, "ab") as f:
            f.write(b"int dist_clang_crash_test_0_1() { return 2; }\n")
        with self.assertRaises(SystemExit):
            crash_test.read_originals([self.path])

    def test_write_atomically(self):
        # The file is replaced at once - never left half-written.
        inode = os.stat(self.path).st_ino
        crash_test.write_file(self.path, b"int b;\n")
        with open(self.path, "rb") as f:
            self.assertEqual(b"int b;\n", f.read())
        self.assertNotEqual(inode, os.stat(self.path).st_ino)
        self.assertEqual(["a.cc"], os.listdir(self.dir.name))


class SignalsTest(unittest.TestCase):
    def test_finally_runs_on_sigterm(self):
        # The sources are restored in "finally" - it runs on SIGTERM and SIGHUP
        # too.
        for signum in (signal.SIGTERM, signal.SIGHUP):
            old = {s: signal.getsignal(s)
                   for s in (signal.SIGTERM, signal.SIGHUP)}
            try:
                crash_test.exit_on_signals()
                restored = False
                with self.assertRaises(SystemExit):
                    try:
                        os.kill(os.getpid(), signum)
                        time.sleep(1)
                    finally:
                        restored = True
                self.assertTrue(restored)
            finally:
                for s, handler in old.items():
                    signal.signal(s, handler)


if __name__ == "__main__":
    unittest.main()
