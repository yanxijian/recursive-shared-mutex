# Agent instructions

This file is the single source of truth for every AI agent working in this
repository: Claude Code, Codex, Cursor, Copilot, Gemini CLI, Aider, or anything
else. `CLAUDE.md`, `.github/copilot-instructions.md` and `.cursor/rules/repo.mdc`
exist only to point here, so there is one place to update.

Rules marked **[CI]** are enforced by `scripts/check_repo_rules.py`, which runs on
every push and pull request. Breaking one fails the build - it is not advice.
Rules marked **[review]** need judgement and cannot be checked mechanically.

---

## What this repository is

A single-header C++17 recursive read/write mutex (`rsm::RecursiveSharedMutex`),
plus a small optional companion header for writer-starvation metrics.

- `include/recursive_shared_mutex/` - the product. Header-only, no dependencies
  beyond the standard library and, on Windows, the Win32 API.
- `tests/` - Catch2 suite, plus two framework-free binaries
  (`backend_contract`, `mutual_exclusion`) that any toolchain can build with one
  compiler call.
- `examples/`, `benchmarks/`, `docs/`.

The recursion state lives in each instance plus a per-thread slot table. The
underlying `std::shared_mutex` is **never re-entered by the same thread** - that
is the invariant the whole design rests on, and `[thread.sharedmutex]` is why it
was built this way. Do not "simplify" it away.

## Build, test, format

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure --timeout 180

python3 scripts/check_repo_rules.py      # repository rules (fast, run before pushing)
clang-format --dry-run -Werror <files>   # the exact list is in .github/workflows/ci.yml
```

Options: `-DRSM_BACKEND_SRWLOCK=...` (via `CMAKE_CXX_FLAGS`), `-DRSM_BUILD_STRESS=ON`,
`-DRSM_BUILD_BENCHMARKS=ON`, `-DRSM_SHARED_SLOT_COUNT=<n>`.

## Hard rules

### R1 - Hand-written C/C++ is UTF-8 with BOM and CRLF **[CI]**

Every `*.h/.hpp/.hxx/.hh/.c/.cc/.cpp/.cxx` in this repository is **UTF-8 with
BOM** and **CRLF**, no exceptions for first-party sources.

- Tools do not do this for you: `clang-format` writes CRLF (`LineEnding: CRLF`)
  but **never** writes a BOM; many editors and patch helpers silently strip it.
- `python3 scripts/check_repo_rules.py --fix` repairs both.
- `.gitattributes` marks them `-text`, so git neither converts them on commit nor
  on checkout - the bytes in the object store are the bytes in your editor.

Why it matters: the header contains `×`, `≈` and `µs` in comments. MSVC assumes
the system code page for a BOM-less file, so a missing BOM shows up as mojibake
in the IDE and, on a non-Western code page, can break parsing.

### R2 - 140 columns, tab-indented, clang-format is the authority **[CI]**

`ColumnLimit: 140`, `UseTab: ForIndentation`, `TabWidth: 4` (see `.clang-format`).
Keep a statement on one line when it fits; do not wrap early "for style", and do
not reflow files you are not otherwise changing.

### R3 - Windows libraries through CMake, never `#pragma comment(lib)` **[CI]**

```cmake
if(WIN32)
	target_link_libraries(the_target PRIVATE dwmapi)   # not "dwmapi.lib"
endif()
```

`#pragma comment(lib, ...)` and `#pragma comment(linker, ...)` only work for
MSVC: CMake/Ninja cannot see the dependency, and clang-cl, GCC and static
analysis all miss it. `#include <windows.h>` in a header is fine - it is the
*link* that belongs in CMake.

### R4 - Headers are self-contained **[CI]**

- `#pragma once` is the first significant line (before it, only comments).
- No `using namespace` at namespace scope; qualify, or use a scoped `using` in a
  function body.
- An include that only one `.cpp` needs belongs in that `.cpp`, not the header.

### R5 - One version, two copies **[CI]**

`project(... VERSION x.y.z)` in `CMakeLists.txt` and
`RSM_VERSION_MAJOR/MINOR/PATCH` in the header must match. They drifted once
(0.1.0 vs the released v0.2.1) and broke `find_package(recursive_shared_mutex 0.2 REQUIRED)`.

### R6 - `CHANGELOG.md` keeps an `## [Unreleased]` section **[CI]**

Behaviour changes, new knobs and fixes go under it (`Added` / `Changed` /
`Fixed`). "No changelog needed" is only true for typo fixes in comments.

### R7 - Line endings follow `.gitattributes` **[CI]**

The standard is declared once, in [`.gitattributes`](.gitattributes):

| Path | Repository and checkout |
|---|---|
| `*.c *.cc *.cpp *.cxx *.h *.hh *.hpp *.hxx` | **UTF-8 BOM + CRLF**, and `-text` so git never converts them (R1) |
| `*.bat *.cmd` | **CRLF in the repository and on checkout**, same rule as C/C++ - cmd.exe expects it |
| everything else | **LF** in the repository and on checkout (`* text=auto eol=lf`) |

`.editorconfig` states the same default and the same C/C++ exception.

Nothing notices a whole-file flip - not the compiler, not clang-format, not git -
and a flip shows up as a rewrite of every line: a 200-line document becomes a
400-line diff without a word changed. So the checker verifies the working tree
against this table, and verifies that `.gitattributes` still declares it.

- New files default to **LF**; only batch files are CRLF.
- Rewriting a file with the wrong endings fails the build; `--fix` normalises the
  ones the table names.
- Changing a file's style means changing `.gitattributes` (and, for a one-off,
  `EOL_POLICY` in `scripts/check_repo_rules.py`) - deliberately, never as a side
  effect of an edit.


### R8 - A translated document stays linked and stays honest **[CI]**

Eight documents have a Simplified Chinese version next to them
(`README.zh-CN.md`, `docs/roadmap.zh-CN.md`, ...). English stays the default; the
translation is a companion, never a replacement.

- Both files carry a language switcher directly under the title:
  `**English** · [简体中文](README.zh-CN.md)` on one side,
  `[English](README.md) · **简体中文**` on the other. The checker verifies the links.
- Each translation records the revision it came from as a content hash:
  `<!-- translated-from: README.md sha256:... -->`. A content hash, not a commit
  id, so a rebase does not invalidate every translation.
- When the English file changes, the checker prints `STALE <file>: ...` and stays
  green. That is deliberate: translating is real work and must not block a merge,
  but it must not go unnoticed either. Re-read the diff, update the translation,
  then update the hash.
- The translated set is `TRANSLATED_DOCS` in `scripts/check_repo_rules.py`. Adding
  or removing a translation means editing that tuple - the point is that the set
  cannot silently lose an entry.

Deliberately **not** translated, and why: `CHANGELOG.md` (history; would need
re-translating on every release), `LICENSE` (legal text), `AGENTS.md` and its
pointers (two copies invite an agent to follow the stale one),
`benchmarks/results/*.md` (generated), and the issue/PR templates (they shape
contributor reports, which are triaged in English).

### R9 - Think before making `try_*` throw **[review]**

`try_lock`, `try_lock_shared`, `unlock` and `unlock_shared` are `Lockable` /
`SharedLockable` members: they must not throw on the "could not acquire" path.
Exhausting the per-thread slot table makes `lock_shared()` throw
`resource_unavailable_try_again` while `try_lock_shared()` returns `false` - keep
that asymmetry, it is deliberate and tested by `tests/backend_contract.cpp`.

### R10 - Never re-enter the backend **[review]**

Any change that lets the *same thread* call the underlying `std::shared_mutex`
twice is undefined behaviour per `[thread.sharedmutex]`. Recursion is intercepted
in `owner_` / `exclusive_count_` / `under_exclusive_` before the backend is
touched. If you are adding a fast path, prove it cannot reach `mtx_.lock()` when
`owner_ == this_thread::get_id()`.

### R11 - Parameter passing **[review]**

Defaults for new and touched APIs; not a mandate to rewrite stable signatures.

| Case | Prefer | Avoid |
|---|---|---|
| Scalars, enums, pointers | by value `T` | needless `const int&` |
| Large read-only | `const T&` | accidental copy |
| Mutate the caller's object | `T&` | out-param disguised as a pointer |
| Exclusive ownership handoff | `std::unique_ptr<T>` by value | naked `new` |

Review priority when asked to review: **correctness > lifetime > threading >
style**. List what you deliberately left alone and why.

### R12 - Do not store references to TLS state **[review]**

The slot table is `thread_local`. A `SharedDepthSlot*` must not outlive the
mutex operation that fetched it, must not be cached in a member, and must not be
handed to another thread.

### R13 - Benchmark numbers carry their machine **[review]**

`benchmarks/results/*.md` are hand-measured on a desktop;
`benchmarks/results/ci-*.md` come from the 4-core CI runner and are only for
run-to-run drift. Never quote a CI number as a performance claim, and never let
the automation overwrite the desktop files.

## Working with this repository

- **Push policy:** work on a branch, open a PR, let CI run. Never force-push
  `main`. Batch related edits into one push: every push costs a full CI cycle
  (~2 minutes), and three pushes for one idea is three minutes paid for nothing.
- **Authorship:** the commit is *authored* by the maintainer, never by the agent.
  An agent-assisted commit carries a `Co-authored-by:` trailer in the body where
  **the agent signs itself** - the actual model that produced the change, not a
  placeholder:

  ```
  Co-authored-by: <model> <noreply@<vendor>>
  ```

  This repository uses

  ```
  Co-authored-by: deepseek-flash v4.1-flash <noreply@deepseek.com>
  ```

  Model name plus the deployment version, which comes from the maintainer: an agent
  cannot read its own deployment identifier, so if a new model is in use the
  maintainer updates this line. Do not invent a version number; if it is unknown,
  write the name alone and say so in the body.

  Why the author is not the agent: a commit authored by an address that belongs to
  nobody is unattributed on GitHub, missing from the contribution graph, and
  invisible to `git log --author`. The trailer keeps the record honest without
  costing the maintainer their history.
- **CI is tiered, and by what the change can break.** `ci.yml` and
  `platform-matrix.yml` filter on *build inputs* (`include/`, `tests/`,
  `examples/`, `CMakeLists.txt`, `.clang-format`, `.gitattributes`): a
  documentation or script change starts no compiler at all. `hygiene.yml` has no
  filter and always runs - it is the only place the rules are checked, and rules
  are exactly what a documentation change can break. The benchmark gate waits for
  `include/**`. A pull request additionally runs a subset of the matrix; the
  exhaustive set runs on `main` and weekly. Do not add a job to the build
  workflows "because it is convenient": it is paid on every push by everyone.
- **`RSM_BACKEND_STD` is the wrong default on MinGW.** libstdc++'s
  `std::shared_mutex` there is built on winpthreads, which grants two exclusive
  owners, so the header auto-selects the native SRWLOCK backend on `__MINGW32__`.
  One weekly job - `expected-fail / mingw winpthreads (red = upstream fixed)` -
  builds the probe binary with `-DRSM_BACKEND_STD` and asserts the contract is
  **still violated**. It is green while the defect exists and turns red when the
  toolchain fixes itself; that red is the instruction to delete the job. Do not
  flip it back into a push-time failure: a permanently red cell teaches everyone
  to ignore red.
- **Benchmark automation** opens no pull requests and commits only
  `benchmarks/results/ci-*.md`. Do not add `gh pr merge` to any workflow.
- **Catch2:** assertions must run on the main thread. A failing `REQUIRE` inside
  a `std::thread` escapes as `Catch::TestFailureException` and terminates the
  process instead of failing the case.
