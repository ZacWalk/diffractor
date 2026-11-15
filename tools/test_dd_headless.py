#!/usr/bin/env python3
# Purpose: Verify the build backend consumes test labels before forwarding application arguments.
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("diffractor_build", Path(__file__).with_name("dd.py"))
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)


class HeadlessLabels(unittest.TestCase):
    def invoke(self, *arguments):
        selected = []
        with patch.object(sys, "argv", ["dd", *arguments]), \
                patch.dict(backend.COMMANDS, {"test": selected.append}), \
                patch("sys.stderr"):
            backend.main()
        return selected

    def test_consumes_headless_label(self):
        result = self.invoke("test", "--label", "^headless$", "--test:*layout*")
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0].rest, ["--test:*layout*"])

    def test_unlabelled_tests_still_work(self):
        self.assertEqual(len(self.invoke("test")), 1)

    def test_empty_selection_fails(self):
        with self.assertRaises(SystemExit) as failure:
            self.invoke("test", "--label", "never")
        self.assertEqual(failure.exception.code, 2)

    def test_invalid_regex_fails(self):
        with self.assertRaises(SystemExit) as failure:
            self.invoke("test", "--label", "[")
        self.assertEqual(failure.exception.code, 2)

    def test_other_commands_reject_label(self):
        with self.assertRaises(SystemExit) as failure:
            self.invoke("build", "--label", "headless")
        self.assertEqual(failure.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
