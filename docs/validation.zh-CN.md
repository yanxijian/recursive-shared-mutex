# 验证

[English](validation.md) · **简体中文**

<!-- translated-from: docs/validation.md sha256:b1fea07e55e3 -->

这页定义本仓库里"已验证"是什么意思、证据怎么记录，以及证据**不**覆盖什么。
soak workflow 的规模按这里定；`verification/soak-history.md` 是账本。

目标是在消费方信任之前先验证过渡组件，而不是让第一个消费方来发现缺陷。固定套件
覆盖"想到的用例"，模型检查覆盖"规范在随机序列下的行为"，soak 覆盖时间与调度家族。

## 现在跑什么

| 层 | 工具 | 位置 |
|---|---|---|
| 后端契约（工具链的 mutex） | `tests/backend_contract.cpp` | 每个 CI cell |
| 固定套件（递归、错误、try 路径） | Catch2，`tests/test_recursive_shared_mutex.cpp` | 每个 CI cell |
| 规范对照影子模型 | `tests/model_check.cpp` | `ctest`、每晚、批次 |
| 库级互斥 | `tests/mutual_exclusion.cpp` | `ctest`、每晚 |
| Sanitizer | ASan/UBSan/TSan 矩阵 | CI、每晚 |
| 打包 | install + `find_package` 消费者 | 每次 CI |
| 平台 | MSVC、clang、MinGW（SRWLOCK 与 winpthreads）、musl、libc++、ARM64、32 位 | CI 矩阵 |

## 两本账，不是一本

覆盖与成本是两个量，分开记。

- **覆盖**——测试自己报的数：`ops`、独占 `entries`、不同 `seed` 数、
  `thread-seconds`（`threads x seconds`）、平台。这些与机器无关，判据用它们。
- **成本**——GitHub 计费的量：job-minutes（每个 job 的墙钟）。一个分片跑 4 线程
  还是 8 线程，成本一样。

不要把"core-hours"当目标：程序测不到它，runner 也不报它；在 4 vCPU 的 runner 上，
线程数是超额订阅的选择，不是资源计数。

## 退出标准

**第一层——每次发布**（0.4.x 已满足）：

- 每个 CI cell 的 `ctest` 绿（v0.4.1 为 30 个测试）。
- 发布周的每晚 soak 绿，seed 由 run id 派生。
- 至少一个 8 job-hours 的已记录批次，零违规。

**第二层——"自证"标准，v1.0 之前或要求消费方信任之前：**

- `model_check`：至少 **10^12 次操作**、至少 **200 个不同 seed**，Linux、Windows、
  ARM64 都零违规。长跑的操作数可以来自一个平台；另外两个平台各自至少要有
  一个已记录批次。
- `mutual_exclusion`：至少 **10^9 次独占进入**，零 overlap、零 reader-with-writer、
  零滞留槽位。
- TSan：至少 **20 thread-hours** clean，包含模型检查。
- ASan/UBSan：完整套件与模型检查。
- 所有错误路径在**并发下**被命中（注入、槽位耗尽、误用组合），不只是单线程。
- 变异测试仍然失败：检测不到被钉死的 `is_held_exclusive()` 或删掉
  `under_exclusive_` 检查的检查器，不算证据。
- 树里没有已知的 flaky 测试。

这些是下限，不是"覆盖完整"的声明。批次按实测速率定规模：
`ops_target / (ops_per_second x shards x minutes x 60)` 得到每片分钟数。账本里已有实测值：
8 分片 × 300 分钟 × 8 线程（40 job-hours）产出 **7.03 x 10^11 次操作**，8 分片 × 60 分钟
产出 **1.44 x 10^11**，整个账本用了 **62 job-hours 达到 1.03 x 10^12**。短分片约每
job-hour 2.1 x 10^10，5 小时分片约 1.76 x 10^10（4 vCPU runner 上长跑会降频）。
下一次定规模前先从历史里读 `ops`。

## 状态（2026-10-18）

依据 `verification/soak-history.md` 里九个已记录批次，Tier 1 与 Tier 2 均已达成：

| 判据 | 结果 |
|---|---:|
| `model_check` 操作数 | **1.03 x 10^12** |
| 不同 seed | **351** |
| 独占进入 | **1.03 x 10^11** |
| TSan | **24.1 thread-hours** |
| 零违规平台 | Linux（长跑）、Windows、ARM64 |
| 变异测试 | 仍按设计失败 |

平台划分并不均匀：长跑的操作数来自 Linux；Windows 与 ARM64 的行是短批次，证明各自
工具链上同样零违规。通过是"未观察到失败"的证据，不是证明；见"不覆盖什么"。


## 怎么跑

- **每晚**——`soak.yml`，自动：`mutual_exclusion` 60 秒、TSan 30 秒、
  `model_check` 四个 seed 加一个 TSan seed，全部由 `GITHUB_RUN_ID` 派生。
  约 8 job-minutes。
- **校准批次**——`soak-batch.yml` 用 `shards=2, minutes=2, seeds_per_shard=2`：
  四个 seed、四个 job-minutes、记录一行。用它测 runner 的 ops/s。
- **里程碑批次**——例如 `shards=4, minutes=120, seeds_per_shard=8`：32 个 seed、
  8 job-hours。`record=true`（默认）会往 `verification/soak-history.md` 追加一行；
  workflow 只提交这个文件。提交用 `GITHUB_TOKEN` 推送，因此不会触发规则检查——
  真正让它保持狭窄的是"拒绝该文件之外的任何路径"这个护栏。
- **TSan 批次**——`tsan=true`：用 `-fsanitize=thread` 构建并跑 `--mutexes 1`
  （见"假阳性"）。仅 Unix。3 分片 × 120 分钟 × 4 线程贡献 24 thread-hours。
- **其它平台**——`runs_on=windows-latest, label=windows` 或
  `runs_on=ubuntu-24.04-arm, label=arm64`；分片相应改用 MSVC 或 g++ 构建。
  这些平台上跑短批次足以证明零违规；长跑留在 Linux，因为同样一个 runner-hour
  能买到最多的 ops。

6 小时 job 上限是约束：`minutes` 保持在 300 或以下。

## 假阳性

红色运行要甄别，绝不静默。第一个已知案例：

- TSan 在 `model_check` 的 phase 1 报 **lock-order inversion**。该阶段同时持有多把
  mutex，但只有**一个线程**，不可能死锁；TSan 看不到阶段边界。TSan 任务用
  `--mutexes 1`（数据竞争检测不需要多锁布局），非 TSan 任务保留 8 把。

任何静默都必须在静默点写注释，并在本节记一行，附可复现的 seed。

## 不覆盖什么

- **跨模块 TLS**：`examples/cross_module_hazard` 的风险需要多 DLL 构建；单进程
  测试证明不了。
- **形式化证明**：这是测试，不是验证。通过是"未观察到失败"的证据，不是证明。
- **性能**：共享 runner 只用于正确性。性能在 `benchmarks/`，遵守 R13。
- **Windows/ARM 覆盖**取决于 CI runner 是否可用；历史记录每行来自哪个平台。

## 历史格式

`verification/soak-history.md` 每个已记录批次一行：

| 日期 | Run | 平台 | 分片 | 分钟 | 线程 | Seeds | Ops | Entries | 结论 |

每行把 run id 与数字关联起来；失败行保留 seed 范围，便于重放。
