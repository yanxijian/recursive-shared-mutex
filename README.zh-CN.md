# recursive-shared-mutex

[English](README.md) · **简体中文**

<!-- translated-from: README.md sha256:903a3e267358 -->

[![CI](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml/badge.svg)](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

C++17 **header-only** 的可重入 `shared_mutex`，用于把代码库从 `std::recursive_mutex` 迁移到读写锁，
而不必立刻把所有嵌套调用拍平。

```cpp
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <mutex>
#include <shared_mutex>

rsm::RecursiveSharedMutex mtx;

void write() {
  std::unique_lock lock(mtx);  // 独占，可重入
  // ...
}

void read() {
  std::shared_lock lock(mtx);  // 共享，可重入
  // ...
}
```

## 特性

- 兼容 `std::unique_lock` / `std::shared_lock`（C++17 的 *SharedMutex* 接口；唯一不满足 `Cpp17Lockable` 字面要求的地方见[限制](#限制)）
- 同时维护**每线程**与**每把锁**的递归计数（POD 槽位表，以 mutex 地址为键；解锁即回收，无堆分配）
- 允许"写 → 读"嵌套；拒绝"读 → 写"升级（抛 `std::system_error`）
- `try_lock` / `try_lock_shared`
- 供调试断言使用的 `is_held_exclusive()`：只有独占属主为真，共享持有者为假——`!try_lock()` 区分不了这两种情况
- 面向分片数组的 `CacheAlignedRecursiveSharedMutex`：状态机相同，实例按 64 字节对齐——
  多把 mutex 相邻时吞吐约 **2×**（见下文）
- header-only，CMake `INTERFACE` 库（`rsm::recursive_shared_mutex`）

## 分片数组：用按 cache line 对齐的变体

默认实例是 24 字节，因此约 2.6 个实例就挤在一条 64 字节 cache line 上。当多把 mutex 相邻存放
（分片数组/vector）时，各分片会对 owner/计数器的写入产生伪共享。8 分片、16 线程实测
（0% / 15% / 50% 写，MSVC 与 MinGW）：

| | 普通 `RecursiveSharedMutex` | `CacheAlignedRecursiveSharedMutex` |
|---|---:|---:|
| 总 ops/s | 13.9–14.3 M | **31.3–33.2 M**（×2.2–2.4） |

```cpp
std::vector<rsm::CacheAlignedRecursiveSharedMutex> shards(8);
```

之所以做成单独的类型而不是默认：把 mutex 内嵌在"每对象一个"的结构体里时，每实例 64 字节是浪费。
可用 `bench_rsm --impl rsm_padded --mode sharded` 复现。

## 编译期开关

| 宏 | 默认值 | 作用 |
|---|---|---|
| `RSM_SHARED_SLOT_COUNT` | `256` | 每线程跟踪的 mutex 上限；一个槽位占 16 B TLS，所以默认每线程 4 KB。线程极多时可调小。 |
| `RSM_VERSION_MAJOR/MINOR/PATCH`、`RSM_VERSION` | `0/3/1` | 头文件版本，供只能看到这个文件的消费者（FetchContent）使用。与 `project(VERSION)` 保持一致。 |
| `RSM_BACKEND_SRWLOCK` | MinGW 上开，其它平台关 | 使用原生 Win32 SRWLOCK 包装作为后端。在 `__MINGW32__` 上自动选中，因为那里的另一个选择不可靠。 |
| `RSM_BACKEND_STD` | 关 | 即使在 MinGW 上也强制 `std::shared_mutex`。仅用于复现 winpthreads 缺陷；每周的 CI 探针正是这么做的。 |
| `RSM_NO_DEBUG_ASSERT` | 关 | 关闭仅 Debug 的"持锁析构"断言。 |
| `RSM_NO_THREAD_MIGRATION_NOTICE` | 关 | 静音 C++20 协程模式下的提示。 |

## 与 `std::recursive_mutex` 的跑分对比

跑分框架见 [benchmarks/README.md](benchmarks/README.md)，跨工具链的完整情况见
[docs/platform-notes.md](docs/platform-notes.md)。

| 纯独占路径（他们的 S7 形态） | `recursive_mutex` | 本库 | 比值 |
|---|---:|---:|---:|
| MSVC (SRWLOCK) | 8.17 M ops/s | 9.53 M ops/s | **1.17×** |
| clang-cl / clang 配 MSVC STL | 8.07 M ops/s | 11.47 M ops/s | **1.42×** |
| MinGW-GCC 13.2 + `RSM_BACKEND_STD`（libstdc++/winpthreads） | 4.99 M ops/s | 0.34 M ops/s | **0.067×** |
| MinGW-GCC 13.2（默认：原生 SRWLOCK） | 4.99 M ops/s | 7.54 M ops/s | **1.52×** |

> **协议：** 8 线程、`try_lock` + `yield` 自旋、嵌套深度 3、5 次交错的 1 秒轮次取中位，跑在一台 32 核
> Windows 主机上。原始 ops/s 与同一张表在 [docs/platform-notes.md](docs/platform-notes.md) §2 与 §6。
>
> 这与 [benchmarks/results](benchmarks/results) 是**两套不同的框架**（3 秒 × 3 次、加解锁之间有实际工作），
> 所以比值不同：那套框架报 1.03×（MSVC）与 1.42×（MinGW 原生后端），这里是 1.17× 与 1.52×。
> 两者结论一致——纯独占路径在噪声范围内持平——但绝对值都不能搬到别的机器上。本机 balanced 场景的
> 运行间噪声可达 250%，因此请把数字当作数量级来读。

- **MSVC / clang 配 MSVC STL**：包装成本约每次加解锁 1.3 ns（4.6 ns vs 裸后端 3.3 ns，均为无争用），而且这个成本
  不再取决于本线程在别处持有多少把共享锁；纯独占开销为 **0.9×–1.2×**，读密集负载总吞吐约提升 **6×–14×**。
- **MinGW-GCC 配 `RSM_BACKEND_STD`**（本库以前默认选它）：0.07×–0.2×，即独占路径 **回退 5×–15×**。
  原因在工具链而不在设计：libstdc++ 走 winpthreads 的 `std::shared_mutex` 比原生 `SRWLOCK` 慢约 11×，
  每次 `thread_local` 访问都走 emutls，而且该后端的 `try_lock()` 甚至连互斥都不保证。
  这就是 MinGW 现在默认原生后端的原因。
- **MinGW-GCC 配原生后端（默认）**：每次独占加解锁从 62.8 → 23.4 ns（2.7×），共享配对 76.4 → 35.1 ns，
  纯独占比值从 0.067× 变成 **1.52×**——比 `std::recursive_mutex` 更快。后端契约测试也重新通过。

### MinGW-w64：原生后端就是默认

```bash
g++ ...                            # 什么都不用加：MinGW 自动拿到原生后端
g++ -DRSM_BACKEND_STD ...          # 退回 std::shared_mutex（在那里不可靠）
```

在 MinGW-w64 上，头文件会自行选择 `RSM_BACKEND_SRWLOCK`，因为另一个选择不只是慢，而是错的。该开关把
`std::shared_mutex` 换成 8 字节的 SRWLOCK 包装（`rsm::detail::SrwLockBackend`，通过
`RecursiveSharedMutex::backend_type` 暴露）。它在 MSVC 上实际上是空操作（MSVC 的 `std::shared_mutex`
本来就*是* SRWLOCK），要求 Windows Vista+，对其它平台没有任何改变。SRWLOCK 在独占模式下不可重入，
但这里没问题：所有递归都在接触后端之前就被拦截了。

用 [benchmarks/run_bench.ps1](benchmarks/run_bench.ps1) 复现，它报告 `-Repeats` 次运行的中位数
（离差为什么重要见 [benchmarks/README.md](benchmarks/README.md)）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks/run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

本文档的英文原版见 [README.md](README.md)；英文始终是默认版本。

## 环境要求

- C++17 编译器（MSVC、GCC、Clang）
- **启用 C++ 异常。** 头文件用 `std::system_error` 报告误用（以及槽位表耗尽）；关闭异常时会以单条 `#error`
  失败，而不是刷出一屏 "cannot use 'throw'" 诊断。`cl.exe` 与 `g++`/`clang++` 默认启用；
  **`clang-cl` 不启用**——需要 `/EHsc`。CMake 目标会为模拟 MSVC 的编译器加上 `/EHsc`。
- CMake ≥ 3.16（用于构建/测试/安装）

## 快速开始

### CMake（推荐）

```cmake
add_subdirectory(recursive-shared-mutex) # 或用 FetchContent
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

FetchContent 示例：

```cmake
include(FetchContent)
FetchContent_Declare(
  recursive_shared_mutex
  GIT_REPOSITORY https://github.com/yanxijian/recursive-shared-mutex.git
  GIT_TAG v0.3.1
)
FetchContent_MakeAvailable(recursive_shared_mutex)
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

### 构建与测试

```bash
cmake -S . -B build -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## 语义（简表）

| 情形 | 行为 |
|------|------|
| 同线程重入 `lock` / `lock_shared` | 允许 |
| 已持独占，再 `lock_shared` | 允许 |
| 已持共享，再 `lock` | 抛 `resource_deadlock_would_occur` |
| 已持共享，再 `try_lock` | 返回 `false` |
| 无所有权时 `unlock`，或在嵌套共享仍持有时 `unlock` | 抛 `operation_not_permitted` |
| 每线程槽位表耗尽 | `lock_shared` 抛 `resource_unavailable_try_again`；`try_lock_shared` 返回 `false` |
| 在不同 OS 线程上解锁 | 不支持（TLS） |
| 跨 DLL/EXE 边界解锁 | 不支持；第二个模块的 `lock_shared` 会重入后端 = UB |

独占递归用实例上的原子 owner id；共享递归用 `thread_local` 的 POD 槽位表，以 mutex 地址为键，
在 `[0, used)` 内保持密集，解锁即回收，线程退出时也不释放。

> **误用会终止进程，而不是抛出。** 上面的 `unlock` 情形会抛 `std::system_error`，但这些调用也会来自
> `std::unique_lock` / `std::shared_lock` 的析构函数，而它们是 `noexcept` 的：一对不匹配的加解锁会让进程
> 结束（`std::terminate`）。这是刻意的快速失败，但**无法被 catch**——详见头文件注释。

本组件的正确性上限取决于它下面的 `std::shared_mutex`。
[tests/backend_contract.cpp](tests/backend_contract.cpp) 会在当前工具链上验证这个契约；
信任一个新编译器/标准库之前先跑它。

迁移指南与测试矩阵见 [docs/migration.md](docs/migration.md)。

## 写者饥饿可观测性（R2）

任何不偏向写者的后端——glibc 的 `pthread_rwlock`、Win32 SRWLOCK——在读压力下都可能让写者饿死。
一个可选的埋点守卫可以在不触碰核心组件的前提下测量写等待与持有时长：

```cpp
#include <recursive_shared_mutex/instrumented_write_lock.hpp>

// 启动时安装一次（生命周期必须覆盖所有守卫）：
rsm::metrics::AtomicStatsRecorder stats;
rsm::metrics::set_recorder(&stats);

// 热路径：
{
    rsm::metrics::instrumented_write_lock guard(mtx); // 记录 wait + hold
    // ... 独占工作 ...
}
// stats.snapshot() -> 供 rsm_write_wait_seconds / rsm_write_hold_seconds 使用
```

未安装 recorder 时零成本；指标与告警契约见 `docs/roadmap.md` 的 R2。

## 限制

- 每线程的 TLS 槽位表是**每模块**一份（header-only）：涉及 DLL/EXE 边界时，同一把 mutex 的
  `lock_shared`/`unlock_shared` 序列必须留在同一个模块的 TLS 域内，或者把 TLS 状态托管到单一导出模块。
  违反这一条是未定义行为，不只是残槽——[examples/cross_module_hazard](examples/cross_module_hazard)
  用真实 DLL/.so 复现了它。
- 一个线程最多同时持有 `kSharedSlotCount`（256）把共享锁（约 4 KB TLS/线程）。第 257 次 `lock_shared`
  抛 `resource_unavailable_try_again`；`try_lock_shared` 返回 `false`。解锁即回收，所以顺序碰任意多把
  mutex 都没问题。
- 不支持锁升级（共享 → 独占）：`lock()` 按设计抛 `resource_deadlock_would_occur`；把读者改成
  `shared_lock` 之前，先审计"先读后写"的调用链。
- 底层的 `std::shared_mutex` 不偏向写者：持续有读者时写者可能饿死（见跑分 S1/S5）。
- 递归深度 > 1 时 `std::condition_variable_any` 不安全：`wait()` 只释放一层，mutex 仍被持有，
  等待线程永远不会被唤醒。
- 后端必须保证互斥。MinGW-w64 GCC 13.2（libstdc++/winpthreads）不保证：`try_lock()` 可能授予两个属主，
  表现为本库 `unlock()` 抛出的虚假 `operation_not_permitted`。这就是 MinGW 默认原生后端、
  `RSM_BACKEND_STD` 需要显式开启的原因；换任何新工具链都要跑
  [tests/backend_contract.cpp](tests/backend_contract.cpp)。细节见 [docs/platform-notes.md](docs/platform-notes.md)。
- **字面上不满足 `Cpp17Lockable`。** 误用时 `unlock()` / `unlock_shared()` 会抛 `std::system_error`，
  而命名要求写的是 *Throws: Nothing*（`[thread.mutex.requirements.mutex]`）。通用算法只会释放自己取得的锁，
  所以 `std::lock`、`std::scoped_lock` 与两个 RAII 包装都正常；但配对错误一旦发生在析构里，进程会被终止（见上文）。
- **MinGW 会把 `<windows.h>` 拉进来。** 默认原身后端需要它，代价是真实的编译时间：在产生这些数字的机器上，
  同一个编译单元 655 ms vs 207 ms（3.2×）。`RSM_BACKEND_STD` 能避开这个包含，但在该工具链上不健全，
  所以没有好的办法绕开。

## 仓库结构

```text
include/recursive_shared_mutex/   # 公共头文件
tests/                            # Catch2 测试 + 无框架回归测试
examples/                         # 用法示例（含跨模块隐患复现）
benchmarks/                       # 驱动、CI 自动化、已入库结果
docs/                             # 设计、迁移、平台记录、路线图
scripts/check_repo_rules.py       # AGENTS.md 承诺由 CI 执行的规则
scripts/tests/                    # 规则检查器自身的测试
cmake/                            # package config
.github/                          # 工作流、issue/PR 模板、CONTRIBUTING / SECURITY / CODE_OF_CONDUCT
AGENTS.md                         # 仓库规则，所有 agent 共用一份
```

## 参与贡献

见 [CONTRIBUTING.md](.github/CONTRIBUTING.md)。较大的设计改动请先开 issue。

**在用 AI agent 干活？** 仓库规则在 [AGENTS.md](AGENTS.md)——一份文件同时服务 Claude Code、Codex、
Cursor、Copilot 以及其它工具，`CLAUDE.md`、`.cursor/rules/repo.mdc` 与
`.github/copilot-instructions.md` 都指向它。其中可由程序检查的部分（UTF-8 BOM + CRLF、140 列、
禁用 `#pragma comment(lib)`、头文件卫生、版本一致、changelog 段，以及译文档的互链不得断开）由 `scripts/check_repo_rules.py`
在每次 push 时于 CI 中执行，所以违规会直接让构建失败，而不是等着评审发现。用
`python3 scripts/check_repo_rules.py --fix` 修掉机械性问题，用 `git config core.hooksPath .githooks`
让检查提前到提交前。

## 安全

见 [SECURITY.md](.github/SECURITY.md)。

## 许可

[MIT](LICENSE) © 2026 yanxijian
