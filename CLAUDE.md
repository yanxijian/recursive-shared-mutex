# Agent instructions

The rules for this repository live in **[AGENTS.md](AGENTS.md)** - one file, so
there is one place to update.

Read it before changing anything. The short version:

- Hand-written C/C++ is **UTF-8 with BOM + CRLF**; `python3 scripts/check_repo_rules.py --fix`
  repairs it.
- **120 columns**, tab-indented, `.clang-format` is the authority.
- Windows libraries go through `target_link_libraries`, never `#pragma comment(lib)`.
- Headers: `#pragma once` first, no `using namespace` at namespace scope.
- `CMakeLists.txt` `VERSION` and `RSM_VERSION_*` must match; `CHANGELOG.md` keeps
  an `## [Unreleased]` section.
- The underlying `std::shared_mutex` must never be re-entered by the same thread
  (`[thread.sharedmutex]`); `try_*` members must not throw on the acquire path.

`python3 scripts/check_repo_rules.py` runs in CI on every push and pull request,
so a violation fails the build rather than waiting for review.
