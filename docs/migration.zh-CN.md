# 迁移指南：`recursive_mutex` → `rsm::RecursiveSharedMutex`

[English](migration.md) · **简体中文**

<!-- translated-from: migration.md sha256:4bac5792a713 -->

## 目标

在**不立刻**把所有依赖 `std::recursive_mutex` 的嵌套调用拍平的前提下，引入读写锁。

更长远的目标：去掉递归，改用普通的 `std::shared_mutex`。

## 实现说明

规范实现：[`include/recursive_shared_mutex/recursive_shared_mutex.hpp`](../include/recursive_shared_mutex/recursive_shared_mutex.hpp)。

| 关注点 | 做法 |
|--------|------|
| 独占递归 | `std::atomic<std::thread::id> owner_` + 仅属主可写的 `exclusive_count_` / `under_exclusive_` |
| 共享递归 | 每线程一张 POD 槽位表，以 mutex 地址（`this`）为键；活跃槽位密集排在 `[0, used)`，深度归零时用"与末位交换"压缩回收 |
| 升级（共享 → 独占） | `lock()` 抛 `resource_deadlock_would_occur`；`try_lock()` 返回 `false` |

共享深度不放 `thread_local unordered_map`：在争用下的 `shared_mutex` 走分配器路径，在某些 MinGW 构建上很脆弱。
堆分配的 `thread_local` 链表也不行：在 `thread_local` 析构中释放节点，会在 MinGW-w64 GCC 13.2 ucrt/posix 的线程退出路径上破坏堆（emutls，已用 gdb 复现）。槽位表是常量初始化的 POD，无析构、无堆分配；槽位以 mutex 地址为键，深度归零即回收，所以活跃集合跟踪的是**当前持有**的共享锁，而不是曾经碰过的 mutex。获取失败（表满，或测试钩子注入失败）都在
**取底层锁之前**抛出，错误不会留下残槽。

## 迁移步骤

1. 把 `std::recursive_mutex` 换成 `rsm::RecursiveSharedMutex`。
2. 写路径继续用 `std::unique_lock`。
3. 审计"同一把锁上先读后写"的调用链（硬性门禁），然后把纯读路径改成 `std::shared_lock`。
4. 修掉共享→独占的嵌套：在最外层改成写锁。

## 硬性规则

- 不允许锁升级（同一把 mutex 上先共享后独占）。
- 不允许跨线程解锁（TLS / owner id）。
- 不要在仍持锁时销毁 mutex。
- 跨模块（DLL/EXE）：TLS 槽位表是**每模块**一份。同一把 mutex 的 `lock_shared`/`unlock_shared`
  序列必须留在同一个模块的 TLS 域内；如果一把 mutex 会被多个模块调用，应把 TLS 状态托管到单一导出模块，
  而不是依赖 header-only 的 TLS。按 `[thread.sharedmutex]`，这是**未定义行为**，不是"残槽"这种边缘情况：
  第二个模块的 `lock_shared()` 找不到槽位，于是在一个**已经**持有共享锁的线程上再次调用底层的 `lock_shared()`。
  SRWLOCK 和读者优先的 glibc/musl rwlock 会静默接受；写者优先的 rwlock 可能让调用线程死锁。
  `examples/cross_module_hazard/` 用真实 DLL/.so 复现了这一点，并已接入 CI。
- 同一把锁上"先读后写"在 `recursive_mutex` 下是串行的，现在会抛 `resource_deadlock_would_occur`；把任何读者改成
  `shared_lock` 之前先审计。
- 写者饥饿：`std::shared_mutex` 不偏向写者。持续有读者时，写者可能饿死（见跑分 S1/S5）；如果写延迟有要求，
  需要在上层加限流/让出。
- 每线程上限：一个线程最多同时持有 `kSharedSlotCount`（256）把共享锁（约 4 KB TLS/线程）。第 257 次
  `lock_shared` 抛 `resource_unavailable_try_again`；`try_lock_shared` 则返回 `false`，因此
  `std::shared_lock(mtx, std::try_to_lock)` 仍然可用。解锁即回收，所以**顺序**碰任意多把 mutex 都没问题。
- 递归深度 > 1 时不能配 `std::condition_variable_any`：`wait()` 只释放一层，mutex 仍被持有，
  等待线程永远不会被唤醒。
- 底层后端必须保证互斥（`try_lock` 最多授予一个属主、写者在临界区内时读者不在），并且必须在调用线程**已持有该
  mutex 共享锁**时拒绝 `try_lock()`。MinGW-w64 GCC 13.2 的 winpthreads 两条都不满足；它已不再是 MinGW 的默认
  后端，`-DRSM_BACKEND_STD` 才能重新选到它。原生 SRWLOCK 同时修掉了正确性缺陷和独占路径 5×–15× 的性能回退
  （见 `docs/platform-notes.md` 第 6 节）。迁移前请在目标工具链上跑 `tests/backend_contract.cpp`。

## 残槽与已被否决的"逃生阀"

当 `unlock_shared()` 在一个并不拥有该槽位的线程（或模块）上执行时，这个共享槽位就**永久残留**：它会抛
`operation_not_permitted`，而真正的属主线程的槽位停在 `{key, depth=1}`。后果是 `try_lock()` 一直返回
`false`、写者持续饿死，直到属主线程正确释放。

曾评估过一个上游 `release_shared_unchecked()`（跳过 TLS 记账，直接调用底层 `unlock_shared()`），结论是**否决**：
它会让属主槽位停在 `{key, depth=1}`，而底层锁其实已经空了；之后属主线程任何 RAII 展开都会**第二次**释放底层锁——
在 SRWLOCK/pthread 路径上无主释放 = 未定义行为 / 堆损坏。只有在"保证属主永不再碰这把 mutex"（线程已终止、
任务已彻底取消、外部守卫已移除）时，它才可能安全。它不是通用的清理路径。

受支持的缓解方式是**归位解锁（homed unlock）**：RAII 守卫记录获取线程；若在外部线程尝试释放，绝不调用库内接口，
只做标记与告警，然后要么把解锁送回属主线程/执行器（交给任务自身的展开逻辑；**不要**从违规处理器里往可能已停滞的队列投递——那是二次死锁），要么接受并上报残槽。参考实现约 20 行：获取时记录 `std::thread::id`，
只有 id 一致时才调用 `unlock_shared()`，析构中绝不抛异常，把亲和性违规交给"标记 + 告警"处理器。

## 测试矩阵

由 `tests/test_recursive_shared_mutex.cpp` 覆盖：

- T1–T8 单线程重入 / 错误路径 / try 路径
- M1–M2 多实例隔离
- C1–C2 并发读者 + 写者
- P1–P2 面对外部持有者的 try 路径
- E1 `lock_shared` 注入获取失败：不留残槽，锁仍可用
- E2 `try_lock_shared` 争用失败干净返回，mutex 仍可用
- E3 多 mutex 的槽位回收 + `kSharedSlotCount` 上限
- E5 `try_lock_shared` 注入获取失败：与 E1 对称
- T12 `[perf]` 持有 64 把共享锁不会拖慢独占路径（且升级尝试仍会抛异常）
- T13 独占慢路径：本线程在别处持共享锁时，争用被写者占有的 mutex 必须阻塞
- T14 外部线程的 `unlock_shared()` 被拒绝，且属主槽位保持完好

无框架回归测试（不依赖 Catch2，一行命令即可编译，跨平台 CI 矩阵在用）：

- `tests/backend_contract.cpp` —— *当前工具链的* `std::shared_mutex` 是否守约？用 8 线程 `try_lock` 风暴加临界区计数，
  外加读者-写者重叠检测。在 MinGW 的 winpthreads 上（即 `RSM_BACKEND_STD`）会失败（约每 1200 次成功获取出现 1 次违规）；
  在 MSVC / clang / clang-cl / 32 位以及 MinGW 的默认原生后端上通过。
- `tests/mutual_exclusion.cpp` —— *库本身*是否让两个写者互斥、读者不与写者重叠？既有套件只断言重入记账与错误路径，
  没有检查这一条。`--seed N` 加每线程操作环形日志，让失败可复现。
- `tests/model_check.cpp` —— *规范本身*：用随机操作序列对照影子模型。单线程阶段是精确的（每个返回值和错误码都预测，
  覆盖多把 mutex）；并发阶段只断言线程自身历史能决定的结果加按 mutex 的互斥，并把停滞变成退出码 3 加环形日志，
  而不是挂死。`--seed` 可复现序列。

故障注入使用 `RSM_ENABLE_TEST_HOOKS` 下才存在的 `fail_next_shared_acquire` 钩子；测试**不要**替换全局分配器。

可选压力测试：`tests/stress_concurrent.cpp`，用 `-DRSM_BUILD_STRESS=ON` 构建，并注册为 `stress_concurrent` 测试。
