#!/usr/bin/env python3
"""Tests of crash_test.py:

  python3 -m unittest tools/stress/crash_test_test.py
"""

import os
import sys
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


if __name__ == "__main__":
    unittest.main()
