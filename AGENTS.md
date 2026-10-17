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
- Do not add a `.gitattributes` that normalises these files to LF.

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

### R7 - A text file keeps its line endings **[CI]**

This repository is deliberately mixed: C/C++ is CRLF (R1), most tooling files are
LF, the older documents are CRLF, and `README.md` plus `docs/platform-notes.md`
are genuinely mixed. Nothing notices a whole-file flip - not the compiler, not
clang-format, not git - and a flip shows up as a rewrite of every line: a 194-line
document becomes a 392-line diff without a word changed.

- New files default to **LF**; the CRLF and mixed files are listed in `EOL_POLICY`
  inside `scripts/check_repo_rules.py`.
- Rewriting a file with the wrong endings fails the build; `--fix` normalises the
  ones the policy names and refuses the mixed ones (it cannot know which line is
  which).
- Moving a file's entry in `EOL_POLICY` is the deliberate way to change its style.

### R8 - Think before making `try_*` throw **[review]**

`try_lock`, `try_lock_shared`, `unlock` and `unlock_shared` are `Lockable` /
`SharedLockable` members: they must not throw on the "could not acquire" path.
Exhausting the per-thread slot table makes `lock_shared()` throw
`resource_unavailable_try_again` while `try_lock_shared()` returns `false` - keep
that asymmetry, it is deliberate and tested by `tests/backend_contract.cpp`.

### R9 - Never re-enter the backend **[review]**

Any change that lets the *same thread* call the underlying `std::shared_mutex`
twice is undefined behaviour per `[thread.sharedmutex]`. Recursion is intercepted
in `owner_` / `exclusive_count_` / `under_exclusive_` before the backend is
touched. If you are adding a fast path, prove it cannot reach `mtx_.lock()` when
`owner_ == this_thread::get_id()`.

### R10 - Parameter passing **[review]**

Defaults for new and touched APIs; not a mandate to rewrite stable signatures.

| Case | Prefer | Avoid |
|---|---|---|
| Scalars, enums, pointers | by value `T` | needless `const int&` |
| Large read-only | `const T&` | accidental copy |
| Mutate the caller's object | `T&` | out-param disguised as a pointer |
| Exclusive ownership handoff | `std::unique_ptr<T>` by value | naked `new` |

Review priority when asked to review: **correctness > lifetime > threading >
style**. List what you deliberately left alone and why.

### R11 - Do not store references to TLS state **[review]**

The slot table is `thread_local`. A `SharedDepthSlot*` must not outlive the
mutex operation that fetched it, must not be cached in a member, and must not be
handed to another thread.

### R12 - Benchmark numbers carry their machine **[review]**

`benchmarks/results/*.md` are hand-measured on a desktop;
`benchmarks/results/ci-*.md` come from the 4-core CI runner and are only for
run-to-run drift. Never quote a CI number as a performance claim, and never let
the automation overwrite the desktop files.

## Working with this repository

- **Push policy:** work on a branch, open a PR, let CI run. Never force-push
  `main`. Batch related edits into one push: every push costs a full CI cycle
  (~2 minutes), and three pushes for one idea is three minutes paid for nothing.
- **CI is tiered:** a pull request runs the cells that could plausibly catch its
  mistake (`ci.yml` on three OSes, `contract-mingw`, the MinGW suite, the
  sanitizers); the exhaustive matrix, the other toolchains and the benchmark gate
  run on `main`, weekly, or when `include/**` changes. Do not make a job run on
  every PR because it is convenient - it is paid on every push by everyone.
- **`RSM_BACKEND_STD` is the wrong default on MinGW.** libstdc++'s
  `std::shared_mutex` there is built on winpthreads, which grants two exclusive
  owners, so the header auto-selects the native SRWLOCK backend on `__MINGW32__`.
  Two CI cells build with `-DRSM_BACKEND_STD` on purpose and stay red; they prove
  the toolchain defect still exists. Do not "fix" that red by deleting the cells -
  if they ever pass, the toolchain fixed itself and the cells can go.
- **Benchmark automation** opens no pull requests and commits only
  `benchmarks/results/ci-*.md`. Do not add `gh pr merge` to any workflow.
- **Catch2:** assertions must run on the main thread. A failing `REQUIRE` inside
  a `std::thread` escapes as `Catch::TestFailureException` and terminates the
  process instead of failing the case.
