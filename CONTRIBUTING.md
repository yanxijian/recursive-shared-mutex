# Contributing

**English** · [简体中文](CONTRIBUTING.zh-CN.md)

Thanks for helping improve **recursive-shared-mutex**.

## Development setup

```bash
git clone https://github.com/yanxijian/recursive-shared-mutex.git
cd recursive-shared-mutex
cmake -S . -B build -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Coding guidelines

- Public API stays in namespace `rsm`
- Keep the library **header-only** and C++17
- Prefer clear failure (`std::system_error`) over silent deadlock for upgrade attempts
- Match `.clang-format` / `.editorconfig` (UTF-8 BOM + CRLF for C/C++ sources on this project)
- Add or update tests for behavior changes
- Update `CHANGELOG.md` under `[Unreleased]` (or the next version section)

## Pull requests

1. Fork and create a feature branch
2. Keep PRs focused (one concern per PR when possible)
3. Ensure CI is green
4. Fill out the PR template

## Reporting bugs

Use the Bug report issue template. Include compiler, OS, minimal repro, and expected vs actual behavior.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
