# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Shared-recursion TLS keys use a unique per-instance `id_` (not the object address)
- Destructor clears the current thread's TLS node for that `id_`

### Added

- Benchmarks vs `std::recursive_mutex` (`benchmarks/`) with MSVC and MinGW-GCC result tables

## [0.1.0] - 2026-10-15

### Added

- Initial `rsm::RecursiveSharedMutex` header-only implementation (C++17)
- CMake INTERFACE package with install/export support
- Catch2 unit tests (single-thread, multi-instance, concurrent, try paths)
- Example `basic_usage`
- Migration guide under `docs/migration.md`
- GitHub Actions CI (Windows / Ubuntu / macOS)
- MIT license and standard community health files

[Unreleased]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.1.0
