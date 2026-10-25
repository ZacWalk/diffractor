"""Registration checks must cover messages moved into libdesktop, not ignore them."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import check_translations as checker


class RegistrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.header = self.root / "app_text.h"
        self.impl = self.root / "app_text.cpp"
        self.platform = self.root / "platform.h"
        self.header.write_text(
            'text_t local = "Local";\n'
            'plural_text count = {"One", "Many"};\n'
            'platform::messages platform_text;\n',
            encoding="utf-8",
        )
        self.platform.write_text('text_t shared = "Shared";\n', encoding="utf-8")
        self.paths = patch.multiple(checker, APP_TEXT_HEADER=self.header, APP_TEXT_IMPL=self.impl)
        self.paths.start()
        self.addCleanup(self.paths.stop)

    def registrations(self, texts="local,\nplatform_text.shared,", plurals="count,"):
        self.impl.write_text(
            "_all_texts = std::vector<std::reference_wrapper<text_t>>\n{\n"
            f"{texts}\n"
            "};\n"
            "_all_plurals = std::vector<std::reference_wrapper<plural_text>>\n{\n"
            f"{plurals}\n"
            "};\n",
            encoding="utf-8",
        )
        return checker.check_app_text_registration(self.platform)

    def test_shared_and_local_members_are_registered(self):
        self.assertEqual([], self.registrations())

    def test_omitted_shared_member_fails(self):
        self.assertIn("Missing from _all_texts (1): platform_text.shared", self.registrations("local,"))

    def test_unknown_shared_member_fails(self):
        self.assertIn(
            "Unknown entries in _all_texts (1): platform_text.typo",
            self.registrations("local,\nplatform_text.shared,\nplatform_text.typo,"),
        )

    def test_duplicate_shared_member_fails(self):
        self.assertIn(
            "Duplicate entries in _all_texts (1): platform_text.shared",
            self.registrations("local,\nplatform_text.shared,\nplatform_text.shared,"),
        )

    def test_shared_member_in_wrong_vector_fails(self):
        self.assertIn(
            "text_t members registered as plurals (1): platform_text.shared",
            self.registrations("local,", "count,\nplatform_text.shared,"),
        )

    def test_expressions_and_unknown_owners_are_not_registrations(self):
        for expression in ("platform_text.shared()", "other.shared", "platform_text.shared + local"):
            with self.subTest(expression=expression):
                self.assertIn("Unexpected entry", self.registrations(expression)[0])

    def test_missing_or_empty_shared_header_fails(self):
        self.registrations()
        missing = checker.check_app_text_registration(self.root / "absent.h")
        self.assertIn("ERROR:", missing[0])
        self.platform.write_text("", encoding="utf-8")
        self.assertIn("No platform messages found", checker.check_app_text_registration(self.platform)[0])

    def test_source_without_composed_messages_needs_no_library_header(self):
        self.header.write_text('text_t local = "Local";\nplural_text count = {"One", "Many"};\n', encoding="utf-8")
        self.assertEqual([], self.registrations("local,"))

    def test_words_are_checked_against_their_shared_header(self):
        with self.header.open("a", encoding="utf-8") as header:
            header.write("str::words words;\n")
        self.platform.with_name("util_strings.h").write_text('text_t month = "Month";\n', encoding="utf-8")
        self.assertEqual([], self.registrations("local,\nplatform_text.shared,\nwords.month,"))
        self.assertIn("Missing from _all_texts (1): words.month", self.registrations())
        self.assertIn(
            "Duplicate entries in _all_texts (1): words.month",
            self.registrations("local,\nplatform_text.shared,\nwords.month,\nwords.month,"),
        )
        self.assertIn(
            "Unknown entries in _all_texts (1): words.typo",
            self.registrations("local,\nplatform_text.shared,\nwords.month,\nwords.typo,"),
        )

    def test_fetched_header_discovery_rejects_ambiguous_builds(self):
        repo = self.root / "app"
        first = repo / "tmp" / "win-build-x64" / "_deps" / "libdesktop-src" / "include" / "libdesktop" / "platform.h"
        first.parent.mkdir(parents=True)
        first.write_text('text_t shared = "Shared";\n', encoding="utf-8")
        first.with_name("util_strings.h").write_text('text_t month = "Month";\n', encoding="utf-8")
        with patch.object(checker, "REPO_ROOT", repo):
            self.assertEqual(first, checker._platform_text_header())
            second = repo / "tmp" / "win-build-x86" / "_deps" / "libdesktop-src" / "include" / "libdesktop" / "platform.h"
            second.parent.mkdir(parents=True)
            second.write_text(first.read_text(encoding="utf-8"), encoding="utf-8")
            second.with_name("util_strings.h").write_text('text_t different = "Different";\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Cannot identify"):
                checker._platform_text_header()


if __name__ == "__main__":
    unittest.main()
