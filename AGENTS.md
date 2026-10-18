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
- `tests/` - Catch2 suite, plus three framework-free binaries
  (`backend_contract`, `mutual_exclusion`, `model_check`) that any toolchain can
  build with one compiler call.
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
python3 -m unittest discover -s scripts/tests   # the checker's own tests
clang-format --dry-run -Werror <files>   # the exact list is in .github/workflows/hygiene.yml
```

Options: `-DRSM_BACKEND_SRWLOCK=...` (via `CMAKE_CXX_FLAGS`), `-DRSM_BUILD_STRESS=ON`,
`-DRSM_BUILD_BENCHMARKS=ON`, `-DRSM_SHARED_SLOT_COUNT=<n>` (program-wide; propagated
by the CMake interface target, or fixed in an installed package - never set this
macro per-target).

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

Nine documents have a Simplified Chinese version next to them
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
that asymmetry, it is deliberate and tested by `tests/test_recursive_shared_mutex.cpp`
(E3, both halves).

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
`benchmarks/results/compare-ci-*.md` comes from the 4-core CI runner and is only
for run-to-run drift. Never quote a CI number as a performance claim, and never
let the automation overwrite the desktop files.

### R14 - Chinese text is not hard-wrapped between characters **[CI]**

A newline inside a Chinese paragraph renders as a space on GitHub. Do not wrap
between two Chinese characters; break after punctuation, a Latin word or inline
code. This applies to the translated documents; English text may wrap normally.
`scripts/check_repo_rules.py` reports the continuation line as `cjk-wrap`.

### R15 - A translated document links to its own language **[CI]**

A Simplified Chinese document links to the `.zh-CN.md` edition of another
translated document, not the English one. The only cross-language link is the
English switcher at the top (the `[English]` link). Fragments must use the translated heading
anchor. `scripts/check_repo_rules.py` reports `lang-link` for a relative link to
an English document whose translation exists.

## Working with this repository

- **Push policy:** work on a branch, open a PR, let CI run. Never force-push
  `main`. Batch related edits into one push: every push costs a full CI cycle
  (~2 minutes), and three pushes for one idea is three minutes paid for nothing.
- **Authorship:** the commit is *authored* by the maintainer, not by the agent.
  Recording that an agent took part is welcome - one trailer at the end of the body
  - but it is a courtesy, not a gate:

  ```
  Co-authored-by: <what you are> [<address>]
  ```

  - Sign as whatever you are actually running as. This repository prescribes no
    model, vendor or version; a later agent must not inherit an earlier one's name.
  - Add a version only when you actually know it. An agent cannot read its own
    deployment identifier, so signing with the name alone is a normal outcome and
    needs no explanation.
  - Prefer an address that reaches nobody (`noreply@...`) over one that might reach
    a person. Leaving the address out is fine too: the line stays readable and is
    simply not counted as a co-author credit.
  - Omitting the trailer is acceptable. The one thing that is not: putting the agent
    in the author field. An address that belongs to nobody is unattributed on
    GitHub, missing from the contribution graph, and invisible to
    `git log --author`.

  (The commits already in this history carry `DeepSeek v4.1-flash` - that is the
  model that actually produced them, not a rule for the next one. One spelling, so
  the history does not read as two different agents.)
- **CI is tiered, and by what the change can break.** `ci.yml` and
  `platform-matrix.yml` filter on *build inputs* (`include/`, `tests/`,
  `examples/`, `CMakeLists.txt`, `.clang-format`, `.gitattributes`): a
  documentation or script change starts no compiler at all. `hygiene.yml` has no
  filter and always runs - it is the only place the rules are checked, and rules
  are exactly what a documentation change can break. The benchmark gate waits for
  `include/**`. A pull request additionally runs a subset of the matrix; the
  exhaustive set runs on `main` and weekly. `soak.yml` runs nightly and on
  demand - the mutual-exclusion driver for a minute, plain and under TSan, plus
  the model checker over four seeds, and a manual `soak-batch.yml` shards
  multi-hour runs into `verification/soak-history.md` - because three seconds is
  not a soak for a concurrency primitive; nothing on the push path waits for it.
  `ci.yml` is **not** a duplicate of `platform-matrix.yml`: it builds the way a
  consumer does - the default CMake generator and the default compiler of each
  OS - while the matrix pins the toolchain and uses Ninja. Both running on
  `main` is deliberate; they run in parallel and the minutes are free, so do not
  deduplicate them without deciding which signal to lose. Do not add a job to
  the build workflows "because it is convenient": it is paid on every push by
  everyone.
- **`RSM_BACKEND_STD` is the wrong default on MinGW.** libstdc++'s
  `std::shared_mutex` there is built on winpthreads, which grants two exclusive
  owners, so the header auto-selects the native SRWLOCK backend on `__MINGW32__`.
  One weekly job - `expected-fail / mingw winpthreads (red = upstream fixed)` -
  builds the probe binary with `-DRSM_BACKEND_STD` and asserts the contract is
  **still violated**. It is green while the defect exists and turns red when the
  toolchain fixes itself; that red is the instruction to delete the job. Do not
  flip it back into a push-time failure: a permanently red cell teaches everyone
  to ignore red.
- **Benchmark automation** commits only `benchmarks/results/compare-ci-*.md`:
  the scheduled run (`update_mode: auto-merge`, the default) pushes that commit
  through the API, so no pull request is involved. The weekly run uses
  `mode=compare`; its healthy-ratio gate reads the previous `compare-ci-*.md`.
  `update_mode: pull-request` does open one and leaves it for a human - that is
  the only mode that touches the pull-request list. Do not add `gh pr merge` to
  any workflow.
- **Bencher tracking is optional and off by default.** `benchmarks.yml` writes
  BMF when the `BENCHER_PROJECT` variable and `BENCHER_API_KEY` secret are set,
  and dry-runs the same file otherwise. It is tracking, not a gate: do not replace
  the compare gate with a Bencher threshold until the noise regime is known, and
  never make a fork PR depend on the token.
- **Catch2:** assertions must run on the main thread. A failing `REQUIRE` inside
  a `std::thread` escapes as `Catch::TestFailureException` and terminates the
  process instead of failing the case.

## Releasing

A release touches five places at once. Miss one and the tree disagrees with
itself: `check_repo_rules.py` catches the version pair (R5) but nothing catches a
README that still advertises the old tag.

1. **Version, both copies** (R5): `project(... VERSION x.y.z)` in `CMakeLists.txt`
   and `RSM_VERSION_MAJOR/MINOR/PATCH` in the header. Patch for documentation, CI
   and tooling; minor for anything a consumer can observe.
2. **`CHANGELOG.md`**: move the `[Unreleased]` body into `## [vX.Y.Z] - YYYY-MM-DD`
   and leave `[Unreleased]` behind, empty (R6). Collapse entries that describe the
   same decision reversed over several commits - the changelog states what is true
   now, not the sequence of attempts.
3. **`README.md` and `README.zh-CN.md`**: the `GIT_TAG` in the FetchContent example
   and the `RSM_VERSION_*` row. Both languages, then refresh the
   `translated-from: README.md sha256:...` hash in the Chinese one, or R8 reports it
   stale.
4. **Tag**, annotated, on the merge commit:

   ```bash
   git tag -a vX.Y.Z -m "vX.Y.Z: <one line on what the release is>"
   git push origin vX.Y.Z
   ```

   Annotated, not lightweight: `git describe` ignores lightweight tags and
   `git push --follow-tags` does not send them.
5. **GitHub Release**, from the changelog section, **bilingual**:

   ```
   <the changelog section, English>

   <details>
   <summary><b>简体中文</b> &nbsp;&middot;&nbsp; Simplified Chinese translation</summary>

   <a full translation, not a summary>

   </details>
   ```

   Title: `<english title> · <中文标题>`. `--latest` only on the newest release;
   GitHub otherwise promotes whichever was created last.

   A **collapsible block, not a jump link**: GitHub renders release, issue and PR
   bodies *without* heading ids, so `[简体中文](#简体中文)` renders as
   `href="#%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87"` and scrolls nowhere. An explicit
   `<a id="x">` survives only as `id="user-content-x"`, which ties the link to a
   sanitiser detail. `<details>/<summary>` passes through unchanged and needs no
   anchor at all - this is the one mechanism verified against the rendered HTML.

   ```bash
   gh release create vX.Y.Z --title "vX.Y.Z - ... · ..." \
     --notes-file notes.md --latest
   ```

Release notes are a **snapshot**: unlike `docs/*.zh-CN.md` they are not tracked by
R8, so a later English change does not mark them stale. Write the Chinese by
translating the English entry - do not translate `CHANGELOG.md` wholesale and copy
paragraphs, and do not translate the words that *describe* corruption
(`Ã—` in an entry about mojibake is content, not damage).

After a history rewrite, tags keep pointing at the old objects: re-point each one at
the equivalent commit on `main` (`git tag -f -a vX.Y.Z <new-commit> -m "..."`, then
force-push the tag). Check with `git merge-base --is-ancestor vX.Y.Z HEAD`.
