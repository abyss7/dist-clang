#!/usr/bin/env python3
"""Tests of incremental.py:

  python3 -m unittest tools/bench/incremental_test.py
"""

import os
import sys
import tempfile
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


if __name__ == "__main__":
    unittest.main()
