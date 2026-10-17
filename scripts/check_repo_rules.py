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

Exit code 0 = clean, 1 = violations. See AGENTS.md for the rule text.
"""

from __future__ import annotations

import argparse
import hashlib
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

COLUMN_LIMIT = 140
TAB_WIDTH = 4


class Report:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.fixed: list[str] = []

    def bad(self, where: str, rule: str, message: str) -> None:
        self.problems.append(f"{where}: [{rule}] {message}")

    def fix(self, where: str, rule: str, message: str) -> None:
        self.fixed.append(f"{where}: [{rule}] {message}")


def tracked_files() -> list[Path]:
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z"],
            cwd=ROOT,
            capture_output=True,
            check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return [p for p in ROOT.rglob("*") if p.is_file() and not skipped(p)]
    files = [Path(p) for p in out.decode("utf-8").split("\0") if p]
    return [ROOT / f for f in files if not skipped(f)]


def staged_files() -> list[Path]:
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    ).stdout
    files = [Path(p) for p in out.decode("utf-8").split("\0") if p]
    return [ROOT / f for f in files if not skipped(f)]


def cpp_only(files: list[Path]) -> list[Path]:
    return [f for f in files if f.suffix.lower() in CPP_SUFFIXES]


def skipped(path: Path) -> bool:
    # Directories only: a file called build_and_run.cmd is not a build directory,
    # and skipping it silently is how a rule stops being enforced.
    parts = path.parts[:-1]
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
# ---------------------------------------------------------------------------
def check_width(paths: list[Path], rep: Report) -> None:
    for path in paths:
        for number, line in enumerate(path.read_bytes().decode("utf-8-sig", "replace").splitlines(), 1):
            width = len(line.replace("\t", " " * TAB_WIDTH).rstrip())
            if width > COLUMN_LIMIT:
                rep.bad(f"{rel(path)}:{number}", "width", f"{width} columns > {COLUMN_LIMIT}")


# ---------------------------------------------------------------------------
# R3 - Windows libraries via CMake, never #pragma comment(lib)
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
# R7 - a text file keeps the line endings it was committed with
#      This repository is deliberately mixed: C/C++ is CRLF (R1), most tooling
#      files are LF, the older documents are CRLF, and two files are genuinely
#      mixed. Nothing notices a whole-file flip - not the compiler, not
#      clang-format, not git - so a stray editor or a careless rewrite can turn a
#      194-line LF document into 392 changed lines without changing a word.
#      Policy: LF unless listed below, and a ""mixed"" entry must stay mixed.
# ---------------------------------------------------------------------------
TEXT_SUFFIXES = {
    ".md",
    ".yml",
    ".yaml",
    ".ps1",
    ".sh",
    ".cmd",
    ".cmake",
    ".in",
    ".txt",
    ".mdc",
    ".py",
    ".editorconfig",
}

EOL_DEFAULT = "lf"
# The standard lives in .gitattributes; this is the working-tree half of it.
# C/C++ is owned by R1 (BOM + CRLF). Batch files are the only real exception:
# cmd.exe expects CRLF. Everything else - documentation, workflows, build files,
# scripts - is LF, which is also .editorconfig's [*] default.
EOL_BY_SUFFIX = {
    ".bat": "crlf",
    ".cmd": "crlf",
    ".sh": "lf",
}
# Per-path overrides go here if a file ever has to differ from its suffix.
EOL_POLICY = {}
# "mixed" is accepted for a path listed here: it means the file must keep both
# endings, and --fix refuses it. No file needs it today.

GITATTRIBUTES_REQUIRED = (
    "* text=auto eol=lf",
    "*.cpp   -text -eol",
    "*.cmd   -text -eol",
    "*.sh    text eol=lf",
)

# Extension-less text files worth checking too (the hook must stay LF or it will
# not run; the formatter config is edited by hand).
# ---------------------------------------------------------------------------
# R8 - a translated document stays linked and stays honest
#      Every document that has a Chinese version must link to it, the Chinese
#      version must link back, and it records which revision of the English file
#      it was translated from. That revision is a content hash, not a commit id,
#      so it survives a rebase; a mismatch is reported as STALE rather than a
#      failure, because translating is not a merge-blocking task.
# ---------------------------------------------------------------------------
TRANSLATED_DOCS = (
    "README.md",
    ".github/CONTRIBUTING.md",
    ".github/SECURITY.md",
    ".github/CODE_OF_CONDUCT.md",
    "benchmarks/README.md",
    "docs/migration.md",
    "docs/platform-notes.md",
    "docs/roadmap.md",
)

TRANSLATION_MARKER = "translated-from:"


def _sha12(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:12]


def check_translations(rep: Report) -> None:
    for english in TRANSLATED_DOCS:
        en_path = ROOT / english
        zh_rel = english[:-3] + ".zh-CN.md"
        zh_path = ROOT / zh_rel
        if not en_path.exists():
            rep.bad(english, "translation", "listed in TRANSLATED_DOCS but missing")
            continue
        if not zh_path.exists():
            rep.bad(zh_rel, "translation", "missing - the translated set must stay complete")
            continue

        en_text = en_path.read_text(encoding="utf-8-sig")
        zh_text = zh_path.read_text(encoding="utf-8-sig")
        zh_leaf = zh_rel.split("/")[-1]

        if zh_leaf not in en_text:
            rep.bad(english, "translation", f"does not link to {zh_leaf}")
        if english.split("/")[-1] not in zh_text:
            rep.bad(zh_rel, "translation", f"does not link back to {english.split('/')[-1]}")
        if TRANSLATION_MARKER not in zh_text:
            rep.bad(zh_rel, "translation", "no `<!-- translated-from: ... sha256:... -->` marker")

        recorded = None
        for line in zh_text.split("\n"):
            if TRANSLATION_MARKER in line:
                for token in line.replace("-->", " ").split():
                    if token.startswith("sha256:"):
                        recorded = token[len("sha256:"):]
        if recorded is None:
            continue
        current = _sha12(en_path)
        if recorded != current:
            print(
                f"STALE  {zh_rel}: translated from {english} at sha256:{recorded}, "
                f"but the English file is now sha256:{current} - re-read the change and update the marker"
            )


TEXT_NAMES = {"LICENSE", ".gitignore", ".gitattributes", ".editorconfig", ".clang-format", "pre-commit"}


def eol_style(raw: bytes) -> str:
    crlf = raw.count(b"\r\n")
    bare_lf = raw.count(b"\n") - crlf
    if crlf and bare_lf:
        return "mixed"
    return "crlf" if crlf else "lf"


def check_line_endings(files: list[Path], rep: Report, do_fix: bool) -> None:
    for path in files:
        if path.suffix.lower() in CPP_SUFFIXES:  # R1 owns those
            continue
        name = path.name
        if path.suffix.lower() not in TEXT_SUFFIXES and name not in TEXT_NAMES:
            continue
        raw = path.read_bytes()
        if not raw:
            continue
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError:
            continue

        actual = eol_style(raw)
        expected = EOL_POLICY.get(rel(path)) or EOL_BY_SUFFIX.get(path.suffix.lower(), EOL_DEFAULT)
        if actual == expected or (expected == "mixed" and actual == "mixed"):
            continue

        if do_fix and expected in ("lf", "crlf"):
            text = raw.decode("utf-8").replace("\r\n", "\n")
            if expected == "crlf":
                text = text.replace("\n", "\r\n")
            path.write_bytes(text.encode("utf-8"))
            rep.fix(rel(path), "eol", f"{actual} -> {expected}")
            continue

        rep.bad(
            rel(path),
            "eol",
            f"line endings are {actual}, this file is {expected} (a whole-file flip "
            f"shows up as a rewrite of every line; if the change is intended, move the "
            f"path in EOL_POLICY of scripts/check_repo_rules.py)",
        )


# ---------------------------------------------------------------------------
# R5 - one version, two copies: CMakeLists VERSION and the header macros
#      (this repository's own; they already drifted once and broke find_package)
# ---------------------------------------------------------------------------
def check_gitattributes(rep: Report) -> None:
    path = ROOT / ".gitattributes"
    if not path.exists():
        rep.bad(".gitattributes", "eol", "missing - the line-ending standard has to be declared there")
        return
    text = path.read_text(encoding="utf-8")
    for required in GITATTRIBUTES_REQUIRED:
        if required not in text:
            rep.bad(".gitattributes", "eol", f"no `{required}` rule - R7 and .gitattributes must agree")


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
    files = staged_files() if args.staged else tracked_files()
    paths = cpp_only(files)
    if not paths and not files:
        print("nothing to check")
        return 0

    check_encoding(paths, rep, args.fix)
    check_width(paths, rep)
    check_pragma_comment(paths, rep)
    check_headers(paths, rep)
    check_line_endings(files, rep, args.fix)
    check_gitattributes(rep)
    check_translations(rep)
    check_version(rep)
    check_changelog(rep)

    for line in rep.fixed:
        print(f"fixed  {line}")
    for line in rep.problems:
        print(f"rule   {line}")

    if rep.problems:
        print(f"\n{len(rep.problems)} violation(s). See AGENTS.md for the rule text.")
        return 1
    scope = "staged files" if args.staged else f"{len(paths)} C/C++ and {len(files)} text files"
    print(f"repository rules: clean ({scope})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
