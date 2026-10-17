# 贡献指南

[English](CONTRIBUTING.md) · **简体中文**

<!-- translated-from: CONTRIBUTING.md sha256:80d561a4fe16 -->

感谢你帮助改进 **recursive-shared-mutex**。

## 开发环境

```bash
git clone https://github.com/yanxijian/recursive-shared-mutex.git
cd recursive-shared-mutex
cmake -S . -B build -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

提交前建议再跑一遍仓库规则检查（CI 也会跑）：

```bash
python3 scripts/check_repo_rules.py
```

## 编码规范

- 公共 API 保持在 `rsm` 命名空间内
- 库保持 **header-only** 且为 C++17
- 升级（共享 → 独占）这类误用，宁可明确失败（`std::system_error`），也不要静默死锁
- 遵循 `.clang-format` / `.editorconfig`（本项目 C/C++ 源文件为 UTF-8 BOM + CRLF）
- 行为变更必须新增或更新测试
- 在 `CHANGELOG.md` 的 `[Unreleased]`（或下一个版本段）下记录改动
- 完整规则见 [AGENTS.md](AGENTS.md)，其中可由程序检查的部分由
  `scripts/check_repo_rules.py` 在 CI 中强制执行

## Pull request

1. Fork 并创建特性分支
2. 保持 PR 聚焦（尽量一个 PR 只做一件事）
3. 确保 CI 全绿
4. 填写 PR 模板

## 报告 bug

请使用 Bug report 模板，并附上编译器、操作系统、最小复现，以及期望行为与实际行为。

## 行为准则

本项目遵循 [Contributor Covenant](CODE_OF_CONDUCT.md)。
