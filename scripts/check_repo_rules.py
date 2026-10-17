#!/usr/bin/env python3
"""Repository rules that are worth enforcing mechanically.

Prose rules rot: an agent reads them, forgets one, and nothing notices. Every
rule in AGENTS.md that *can* be checked by a program is checked here, so a
violation fails CI instead of relying on review. Rules that need judgement stay
in AGENTS.md and are marked [review].

Run locally:
    python3 scripts/check_repo_rules.py           # check
    python3 scripts/check_repo_rules.py --fix     # fix the mechanical ones
    python3 scripts/check_repo_rules.py --staged  # only git-staged files (pre-commit)

Exit code 0 = clean, 1 = violations.

Derived from the Cursor rules in D:\\Codes\\Volition\\.cursor\\rules, adapted to
this repository: see AGENTS.md for which ones were imported, changed, or
rejected, and why.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOM = b"\xef\xbb\xbf"

CPP_SUFFIXES = {".h", ".hpp", ".hxx", ".hh", ".c", ".cc", ".cpp", ".cxx"}

# Directories that never count as first-party source.
SKIP_PARTS = {
    "build",
    ".git",
    "_deps",
    "external",
    "third_party",
    "vcpkg_installed",
    "out",
}
SKIP_PREFIXES = ("build-", "build_", "cmake-build")

COLUMN_LIMIT = 120
TAB_WIDTH = 4


class Report:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.fixed: list[str] = []

    def bad(self, where: str, rule: str, message: str) -> None:
        self.problems.append(f"{where}: [{rule}] {message}")

    def fix(self, where: str, rule: str, message: str) -> None:
        self.fixed.append(f"{where}: [{rule}] {message}")


def tracked_cpp_files() -> list[Path]:
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z"],
            cwd=ROOT,
            capture_output=True,
            check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return [
            p
            for p in ROOT.rglob("*")
            if p.suffix.lower() in CPP_SUFFIXES and not skipped(p)
        ]
    files = [Path(p) for p in out.decode("utf-8").split("\0") if p]
    return [ROOT / f for f in files if f.suffix.lower() in CPP_SUFFIXES and not skipped(f)]


def staged_cpp_files() -> list[Path]:
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    ).stdout
    files = [Path(p) for p in out.decode("utf-8").split("\0") if p]
    return [ROOT / f for f in files if f.suffix.lower() in CPP_SUFFIXES and not skipped(f)]


def skipped(path: Path) -> bool:
    parts = path.parts
    if any(part in SKIP_PARTS for part in parts):
        return True
    return any(part.startswith(SKIP_PREFIXES) for part in parts)


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


# ---------------------------------------------------------------------------
# R1 - hand-written C/C++ is UTF-8 with BOM and CRLF
#      (imported from cpp-source-encoding.mdc; the repository already declared
#      this in .clang-format but nothing enforced it)
# ---------------------------------------------------------------------------
def check_encoding(paths: list[Path], rep: Report, do_fix: bool) -> None:
    for path in paths:
        raw = path.read_bytes()
        if not raw:
            continue
        has_bom = raw.startswith(BOM)
        bare_lf = raw.count(b"\n") - raw.count(b"\r\n")
        if has_bom and bare_lf == 0:
            continue

        if do_fix:
            text = raw.decode("utf-8-sig", errors="replace")
            body = text.replace("\r\n", "\n").replace("\n", "\r\n")
            path.write_bytes((BOM if not has_bom else b"") + body.encode("utf-8"))
            details = []
            if not has_bom:
                details.append("added BOM")
            if bare_lf:
                details.append(f"normalised {bare_lf} bare LF to CRLF")
            rep.fix(rel(path), "encoding", ", ".join(details))
        else:
            if not has_bom:
                rep.bad(rel(path), "encoding", "missing UTF-8 BOM")
            if bare_lf:
                rep.bad(rel(path), "encoding", f"{bare_lf} LF line ending(s) instead of CRLF")


# ---------------------------------------------------------------------------
# R2 - nothing wider than .clang-format's ColumnLimit
#      (from cpp-column-limit.mdc, with this repository's 120 instead of 140)
# ---------------------------------------------------------------------------
def check_width(paths: list[Path], rep: Report) -> None:
    for path in paths:
        for number, line in enumerate(path.read_bytes().decode("utf-8-sig", "replace").splitlines(), 1):
            width = len(line.replace("\t", " " * TAB_WIDTH).rstrip())
            if width > COLUMN_LIMIT:
                rep.bad(f"{rel(path)}:{number}", "width", f"{width} columns > {COLUMN_LIMIT}")


# ---------------------------------------------------------------------------
# R3 - Windows libraries via CMake, never #pragma comment(lib)
#      (imported verbatim from cmake-windows-libs.mdc)
# ---------------------------------------------------------------------------
PRAGMA_LINK = re.compile(r"#\s*pragma\s+comment\s*\(\s*(lib|linker)", re.IGNORECASE)


def check_pragma_comment(paths: list[Path], rep: Report) -> None:
    for path in paths:
        for number, line in enumerate(path.read_bytes().decode("utf-8-sig", "replace").splitlines(), 1):
            if PRAGMA_LINK.search(line):
                rep.bad(
                    f"{rel(path)}:{number}",
                    "cmake-libs",
                    "#pragma comment(lib/linker, ...) - link through target_link_libraries instead",
                )


# ---------------------------------------------------------------------------
# R4 - headers are self-contained: #pragma once, no `using namespace` at scope
#      (this repository's own convention; also keeps headers includable twice)
# ---------------------------------------------------------------------------
def first_significant_line(lines: list[str]) -> str:
    """First line that is neither blank nor part of a comment."""
    in_block = False
    for line in lines:
        text = line.strip()
        if in_block:
            if "*/" in text:
                in_block = False
                text = text.split("*/", 1)[1].strip()
                if text:
                    return text
            continue
        if not text:
            continue
        if text.startswith("//"):
            continue
        if text.startswith("/*"):
            if "*/" not in text:
                in_block = True
                continue
            text = text.split("*/", 1)[1].strip()
            if not text:
                continue
        return text
    return ""


def check_headers(paths: list[Path], rep: Report) -> None:
    for path in paths:
        if path.suffix.lower() not in {".h", ".hpp", ".hxx", ".hh"}:
            continue
        lines = path.read_bytes().decode("utf-8-sig", "replace").splitlines()
        first = first_significant_line(lines)
        if first != "#pragma once":
            rep.bad(rel(path), "header", f"first significant line is {first!r}, expected '#pragma once'")
        for number, line in enumerate(lines, 1):
            stripped = line.strip()
            if stripped.startswith("using namespace") and not line[:1] in ("\t", " "):
                rep.bad(f"{rel(path)}:{number}", "header", "`using namespace` at namespace scope")


# ---------------------------------------------------------------------------
# R5 - one version, two copies: CMakeLists VERSION and the header macros
#      (this repository's own; they already drifted once and broke find_package)
# ---------------------------------------------------------------------------
def check_version(rep: Report) -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    header = (ROOT / "include/recursive_shared_mutex/recursive_shared_mutex.hpp").read_text(encoding="utf-8-sig")

    cm = re.search(r"project\s*\([^)]*?VERSION\s+([0-9]+)\.([0-9]+)\.([0-9]+)", cmake, re.DOTALL)
    if not cm:
        rep.bad("CMakeLists.txt", "version", "project(... VERSION x.y.z) not found")
        return
    macros = {}
    for name in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+RSM_VERSION_{name}\s+(\d+)", header)
        if not m:
            rep.bad("include/recursive_shared_mutex/recursive_shared_mutex.hpp", "version", f"RSM_VERSION_{name} missing")
            return
        macros[name] = int(m.group(1))

    cmake_tuple = (int(cm.group(1)), int(cm.group(2)), int(cm.group(3)))
    header_tuple = (macros["MAJOR"], macros["MINOR"], macros["PATCH"])
    if cmake_tuple != header_tuple:
        rep.bad(
            "CMakeLists.txt vs header",
            "version",
            f"CMake {cmake_tuple} != RSM_VERSION_* {header_tuple} - bump both",
        )


# ---------------------------------------------------------------------------
# R6 - a change log entry exists, so agents do not silently change behaviour
# ---------------------------------------------------------------------------
def check_changelog(rep: Report) -> None:
    path = ROOT / "CHANGELOG.md"
    if not path.exists():
        rep.bad("CHANGELOG.md", "changelog", "file missing")
        return
    if "## [Unreleased]" not in path.read_text(encoding="utf-8"):
        rep.bad("CHANGELOG.md", "changelog", "no `## [Unreleased]` section")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="fix the mechanical violations")
    parser.add_argument("--staged", action="store_true", help="only check git-staged files (pre-commit)")
    args = parser.parse_args()

    rep = Report()
    paths = staged_cpp_files() if args.staged else tracked_cpp_files()
    if not paths:
        print("no C/C++ files to check")
        return 0

    check_encoding(paths, rep, args.fix)
    check_width(paths, rep)
    check_pragma_comment(paths, rep)
    check_headers(paths, rep)
    check_version(rep)
    check_changelog(rep)

    for line in rep.fixed:
        print(f"fixed  {line}")
    for line in rep.problems:
        print(f"rule   {line}")

    if rep.problems:
        print(f"\n{len(rep.problems)} violation(s). See AGENTS.md for the rule text.")
        return 1
    scope = "staged files" if args.staged else f"{len(paths)} files"
    print(f"repository rules: clean ({scope})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
