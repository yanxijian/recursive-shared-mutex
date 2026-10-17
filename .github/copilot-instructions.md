# Copilot instructions

The rules and conventions for this repository live in **[AGENTS.md](../AGENTS.md)**
at the repository root - one source of truth for every agent, not a copy per tool.

Summary of what is machine-enforced (`python3 scripts/check_repo_rules.py`, run in
CI on every push and pull request):

- C/C++ sources are **UTF-8 with BOM + CRLF** (`--fix` repairs it).
- **120 columns**, tab-indented, `.clang-format` is the authority.
- No `#pragma comment(lib|linker, ...)`: link Windows libraries via
  `target_link_libraries`.
- Headers: `#pragma once` first, no `using namespace` at namespace scope.
- `project(VERSION)` must equal `RSM_VERSION_MAJOR/MINOR/PATCH`.
- `CHANGELOG.md` keeps an `## [Unreleased]` section.

Correctness rules that need judgement (see AGENTS.md for the reasoning):

- The underlying `std::shared_mutex` is never re-entered by the same thread;
  recursion is intercepted before the backend is touched (`[thread.sharedmutex]`).
- `try_lock` / `try_lock_shared` / `unlock` / `unlock_shared` must not throw on
  the "could not acquire" path.
- Nothing may hold a pointer into the per-thread slot table across operations or
  across threads.
