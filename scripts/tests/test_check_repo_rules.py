#!/usr/bin/env python3
"""Tests for scripts/check_repo_rules.py.

The checker is the only thing that turns AGENTS.md's mechanical rules into a
build failure, so it is worth testing on its own: `--fix` once silently stripped
the UTF-8 BOM it had just accepted (the repair branch re-encoded after
`utf-8-sig` had eaten the BOM and only put one back for files that never had it).
Nothing noticed, because the script had no tests.

Run: python3 -m unittest discover -s scripts/tests
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_repo_rules as rules

BOM = rules.BOM


class EncodingTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write(self, name: str, data: bytes) -> Path:
        path = self.dir / name
        path.write_bytes(data)
        return path

    def test_fix_keeps_the_bom_while_normalising_line_endings(self) -> None:
        path = self.write("mixed.hpp", BOM + b"int a;\r\nint b;\n")
        report = rules.Report()
        rules.check_encoding([path], report, do_fix=True)

        data = path.read_bytes()
        self.assertTrue(data.startswith(BOM), "--fix stripped the BOM it had just accepted")
        self.assertEqual(data.count(b"\n"), data.count(b"\r\n"), "bare LF survived the repair")

        after = rules.Report()
        rules.check_encoding([path], after, do_fix=False)
        self.assertEqual(after.problems, [], "one --fix run must leave the file clean")

    def test_fix_is_idempotent(self) -> None:
        path = self.write("mixed.hpp", BOM + b"int a;\r\nint b;\n")
        rules.check_encoding([path], rules.Report(), do_fix=True)
        second = rules.Report()
        rules.check_encoding([path], second, do_fix=True)
        self.assertEqual(second.fixed, [])

    def test_fix_adds_a_bom_to_a_bom_less_file(self) -> None:
        path = self.write("plain.hpp", b"int a;\r\n")
        rules.check_encoding([path], rules.Report(), do_fix=True)
        self.assertTrue(path.read_bytes().startswith(BOM))

    def test_check_reports_missing_bom_and_bare_lf(self) -> None:
        path = self.write("plain.hpp", b"int a;\nint b;\r\n")
        report = rules.Report()
        rules.check_encoding([path], report, do_fix=False)
        messages = " ".join(report.problems)
        self.assertIn("missing UTF-8 BOM", messages)
        self.assertIn("LF line ending", messages)


class LineEndingStyleTest(unittest.TestCase):
    def test_style(self) -> None:
        self.assertEqual(rules.eol_style(b"a\r\nb\r\n"), "crlf")
        self.assertEqual(rules.eol_style(b"a\nb\n"), "lf")
        self.assertEqual(rules.eol_style(b"a\r\nb\n"), "mixed")


class ChangelogDefinitionTest(unittest.TestCase):
    def test_heading_without_definition_is_reported(self) -> None:
        text = "## [Unreleased]\n\n## [v0.1.0] - 2026-01-01\n\n[Unreleased]: https://example.com\n"
        self.assertEqual(rules.missing_changelog_definitions(text), ["v0.1.0"])

    def test_every_heading_defined(self) -> None:
        text = "## [Unreleased]\n\n## [v0.1.0] - 2026-01-01\n\n[v0.1.0]: https://example.com\n[Unreleased]: https://example.com\n"
        self.assertEqual(rules.missing_changelog_definitions(text), [])


if __name__ == "__main__":
    unittest.main()

class ChineseWrappingTest(unittest.TestCase):
    def test_flags_character_boundary_break(self) -> None:
        self.assertEqual(rules.cjk_wrap_lines("中文\n中文"), [2])

    def test_allows_break_after_punctuation(self) -> None:
        self.assertEqual(rules.cjk_wrap_lines("中文，\n中文"), [])

    def test_skips_fenced_code_and_tables(self) -> None:
        text = "```\n中文\n中文\n```\n| 中文 |\n| 中文 |\n"
        self.assertEqual(rules.cjk_wrap_lines(text), [])

    def test_skips_headings(self) -> None:
        self.assertEqual(rules.cjk_wrap_lines("# 标题\n中文"), [])

class LanguageLinkTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        (self.dir / "README.md").write_text("", encoding="utf-8")
        (self.dir / "README.zh-CN.md").write_text("", encoding="utf-8")

    def test_flags_link_to_english_document(self) -> None:
        got = rules.missing_translation_links("[Read](README.md)", self.dir)
        self.assertEqual(got, [(1, "README.md", "README.zh-CN.md")])

    def test_ignores_switcher_and_external_links(self) -> None:
        text = "[English](README.md)\n[site](https://example.com)\n[local](README.zh-CN.md)\n"
        self.assertEqual(rules.missing_translation_links(text, self.dir), [])

    def test_ignores_links_inside_code(self) -> None:
        text = "```\n[Read](README.md)\n```\n"
        self.assertEqual(rules.missing_translation_links(text, self.dir), [])

