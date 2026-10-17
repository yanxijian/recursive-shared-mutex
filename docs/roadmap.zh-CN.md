# 路线图

[English](roadmap.md) · **简体中文**

<!-- translated-from: roadmap.md sha256:ec354da1fcd6 -->

RecursiveSharedMutex 是一个**过渡组件**：它的作用是把代码库从 `std::recursive_mutex` 带到读写锁，
直到代码库可以去递归化。本路线图覆盖剩余的生命周期。

## R1 - 绑定约束封条 + 编译期迁移提示（立即，仅注释/预处理器）

状态：已**落地**在头文件中（THREAD-BINDING CONSTRAINT 块 + 编译期提示）。验收已在 MinGW-w64 GCC 13.2 上验证：
`-std=c++17` 构建不输出任何东西，`-std=c++20` 输出提示，边界测试套件不变。

1. 在头文件中加入醒目的 OS 线程绑定约束块。措辞必须准确——违规的机制是快速失败（fail-fast）+ 残槽，
   而不是内存损坏：

- 本组件绑定到操作系统原生线程；所有共享递归记账都在每线程 TLS 中。
- 凡是任务可能在不同物理线程上恢复的场景一律**禁止**：可迁移线程的协程、工作窃取线程池（TBB 等）、
  可迁移的用户态纤程。
- **安全**：总在获取线程上恢复的有栈协程/纤程；执行器保证同线程恢复时的 C++20 协程。
- 违规后果：从外部线程调用 `unlock_shared()` 会抛 `operation_not_permitted`；残槽会让写者饿死；
  从析构函数中逃逸的异常会终止进程。（这里的内存损坏**不是**该机制——那是另一个已修复的 emutls TLS 析构缺陷，
  见 v0.2.0。调试时不要混为一谈。）

2. 编译期迁移提示，由特性测试宏触发（C++17 构建看不到任何东西；只有将来升级到启用协程时才会触发）：

```cpp
#if defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L \
    && !defined(RSM_NO_THREAD_MIGRATION_NOTICE)
#pragma message("RecursiveSharedMutex is bound to OS threads: a task that " \
                "resumes on a different physical thread will strand its "   \
                "shared slot and starve writers. Same-thread resume is "    \
                "safe. See the thread-binding notes above.")
#endif
```

- 只是**提示级**（`#pragma message`），不是 `#warning`：启用协程语言模式本身不是误用；同线程恢复是合法的。
- 关闭开关：`-DRSM_NO_THREAD_MIGRATION_NOTICE`。
- 诚实的边界：编译期无法检测线程迁移或工作窃取执行器（那是运行时属性），而 C++17 **并不**排除可在 C++17 使用的
  有栈协程/纤程或 TBB 风格线程池——那些只能由运行时亲和性守卫（验证工具）覆盖。

验收：只改注释/预处理器的变更；边界测试套件不变；用 `-std=c++20` 编译一个 TU 能看到提示，`-std=c++17` 看不到。

## R2 - 写者饥饿可观测性（短期，消费方项目）

状态：**库侧已落地**（v0.2.1+）：可选的 `recursive_shared_mutex/instrumented_write_lock.hpp` 提供 RAII 守卫、
`Recorder` 汇接口，以及内置的 `AtomicStatsRecorder`（仅 count/total/max，纯原子操作）。未安装 recorder 时零成本。
R2 剩余的部分在消费方项目：把 `Recorder` 接到指标栈，并对 p99 写等待告警。

经验依据：MinGW 跑分 S1/S5 的写吞吐在读压力下会塌到个位数；glibc 的 `pthread_rwlock` 默认是
`PTHREAD_RWLOCK_PREFER_READER_NP`，所以 Linux 生产环境同样暴露。

在守卫层埋点（保持库本身零开销）：

- `rsm_write_wait_seconds` 直方图：在 `lock()` / `try_lock` 重试循环里花的时间，按 mutex 站点分。
- `rsm_write_hold_seconds` 直方图：独占临界区的持续时间。
- 告警：p99 写等待超过阈值并持续 N 分钟。

观察到饥饿后的缓解手段：写者批处理、限制读者速率、分片以降低单锁上的读者争用，或 R4。

## R3 - 去递归化工程（中期，消费方项目）

接口/实现分离，按调用图顺序逐模块推进：

- 公共 API 在最外层取 `WriteLock`（`std::unique_lock`）或 `ReadLock`（`std::shared_lock`）。
- 私有核心函数（`xxx_NoLock`）不加锁，可以自由互相调用；它们必须写明调用方需要持有什么模式的锁。

安全护栏：

- 独占核心用仅 Debug 的持锁断言：`assert(!mtx.try_lock())` 仅当调用方已独占持有该锁时才成立。
- 共享核心无法这样断言（`try_lock_shared` 合法地会成功）；它们依赖调用图纪律 + 每一步之后都跑边界测试套件。
- "先读后写"的链条在这种模式下自然消失：最外层决定模式，因此升级尝试不可能发生。

收益：可重入是本组件**所有**已记录约束的**根因**（TLS 域规则、256 槽上限、禁止线程迁移、跨模块共享路径规则）。
一旦不再有重入，就可以用 `std::shared_mutex` 替换整个组件，这些约束也随之消失。

退出条件：在消费方代码库中把 RecursiveSharedMutex 标记为已弃用；库本身仍对其它使用者可用。

## R4 - （可选，库侧）可插拔的公平后端

状态：**完成一半**。后端今天已经可插拔——`RSM_BACKEND_SRWLOCK`（MinGW 上的默认）与 `RSM_BACKEND_STD`
在原生 SRWLOCK 包装和 `std::shared_mutex` 之间选择，`RecursiveSharedMutex::backend_type` 暴露了选择结果。
缺的是**写者优先**的实现。

只有在 R2 于生产环境证明确实饥饿时才做：实现那个公平 rwlock，暴露同样的 `SharedMutex` 接口。
不要凭猜测提前实现——不公平是标准权衡，而公平后端会让读者付出代价。

## 本路线图写成之后已落地的东西

- 后端契约测试（`tests/backend_contract.cpp`、`tests/mutual_exclusion.cpp`），无框架依赖，
  任意工具链一行编译命令即可检查。
- [`.github/workflows/platform-matrix.yml`](../.github/workflows/platform-matrix.yml) 里的平台矩阵，
  外加一个每周运行的 `expected-fail` 探针，用来持续证明 MinGW winpthreads 缺陷仍然存在：
  缺陷存在时是绿的，上游修好那天变红。（这里不写格数：那句话里的数字已经过期两次。）
- MinGW 默认使用原生 SRWLOCK 后端（v0.3.0）。
- 编译期开关：`RSM_SHARED_SLOT_COUNT`、`RSM_VERSION`、`RSM_NO_DEBUG_ASSERT`、`RSM_BACKEND_STD`。
- 面向分片数组的 `rsm::CacheAlignedRecursiveSharedMutex`（在那种场景下约 2×）。
- 自动刷新 `benchmarks/results/ci-*.md` 的跑分流水线，以及一套在 CI 中检查的仓库规则（`AGENTS.md`）。
- `benchmarks/results/*.md` 已在 v0.3.0 上重测，带中位数和离差行。

## 待办

- R2 剩下的那一半在消费方项目：把 `Recorder` 接到指标栈，并对 p99 写等待告警。
- 在真正的 ARM64 硬件上测量，而不是 CI 的预览 runner。
- 若 R2 在生产中显示出饥饿，则做写者优先后端（R4）。
