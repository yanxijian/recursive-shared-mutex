# 与 `std::recursive_mutex` 的跑分对比

[English](README.md) · **简体中文**

<!-- translated-from: README.md sha256:4f48bd60d417 -->

在嵌套加锁、约 4 MiB 工作集的条件下，对比 `rsm::RecursiveSharedMutex`（除特别说明外读者走共享锁）与
`std::recursive_mutex`。

## 运行

```powershell
# 可移植：只需 PATH 上有 cl.exe 或 g++，不硬编码 Visual Studio 路径。
#   -Toolchain msvc|mingw|both   -Seconds 3   -Repeats 3   -OutDir results
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

每个 (场景, 实现) 组合都在**独立进程**中运行，这样 MinGW `shared_mutex` 的压力测试不会污染同进程后续的配置。
每个组合跑 `-Repeats` 次，报告取**中位数**；共享机器上单次运行的噪声太大，不适合用来比较。

跑分驱动通过 CMake 构建（`-DRSM_BUILD_BENCHMARKS=ON`，目标 `bench_rsm`），因此不依赖 Visual Studio 装在哪里。

## 自动化

[`.github/workflows/benchmarks.yml`](../.github/workflows/benchmarks.yml) 在固定的 `windows-2022` runner 上跑这些场景。

- **每周（周一 03:00 UTC）**及手动触发：默认 `mode=compare`，两个实现放在同一个进程里交替运行（ABBA），
  同时写出 `results/compare-ci-<toolchain>.md`（下一次的 gate 基线）和给 Bencher 的 BMF。默认
  `update_mode: auto-merge` 只提交 `compare-ci-*.md`，且要求每个改动路径都匹配这个模式；`pull-request`
  会开 PR 留待评审，`commit` 提交到当前分支，`none` 只测量。
- **改动 `include/**` 的 PR**：短跑一次并与已提交的 `compare-ci-msvc.md` 比较。只有 S6（分片）和
  S7（纯独占）会卡住 job，阈值 20%。在 4 核 runner 上"读并行"类场景按构造贴着 1.0×，能反映包装成本
  变化的是 S6/S7。
- 工作流从不合并 pull request，包括 `pull-request` 模式自己开的那个。唯一的自动合并是 job 几秒前创建
  的结果提交，且要通过上面的路径检查；gate job 只读。

注意事项：

- 绝对 ops/s 属于产出它的机器。要看的是同一次运行内相对 `recursive_mutex` 的比值。
- 闸门只使用同一 runner 上 CI 产出的报告。每份 compare 报告会记录有多少轮基线是健康的
  （达到本轮最好成绩的 80% 以内）；健康轮次少于 3 的场景跳过，不判失败。
- GitHub runner 只有 4 核。共享锁要读者多于核数才有收益，所以 CI 上 S1–S5 接近 1.0×，而 32 核桌面
  能拉开明显差距。CI 用于同硬件上的漂移检测，不是性能结论；少于 8 核时报告会自己注明。
- `update_mode: pull-request` 需要在 Settings > Actions > General 打开
  *Allow GitHub Actions to create and approve pull requests*；auto-merge 不需要任何设置。

在大机器上让指标有意义之前，先本地跑：

```powershell
# 例如在 32 核桌面上
powershell -NoProfile -File benchmarks/run_bench.ps1 -Toolchain msvc -Seconds 3 -Repeats 3
```

## 持续跟踪（可选，Bencher）

`run_bench.ps1 -Json <path>` 输出 [Bencher Metric Format](https://bencher.dev/docs/reference/bencher-metric-format/)：
每个（工具链, 场景, 实现）一个 JSON 对象，含 `throughput`（多次重复的中位数与 min/max）。
`benchmarks.yml` 在测量后把该文件交给 `bencher run --adapter json`。

这**只是跟踪**。gate 仍是上面的 `-RegressionPct` 比较，CI 不设置阈值。没有仓库变量
`BENCHER_PROJECT` 和 secret `BENCHER_API_KEY` 时，步骤用 `--dry-run` 把报告打到日志，
fork 或新克隆无需账号即可验证格式。

启用方法：先创建项目（发送路径要求项目已存在），再打开项目的 **Keys** 页创建 key——
`bencher_run_*` 的值只显示一次，key 的详情页之后只有元数据。把 `BENCHER_PROJECT`（项目 slug）
设为仓库变量、`BENCHER_API_KEY` 设为 secret。先不要设阈值。共享 runner 的墙钟噪声有几十个
百分点，阈值要等积累几周历史才有意义；真要硬性 gate，指令数基准更合适。

## 这些数字有多准？

不准。每份报告都带一行 `Run-to-run spread`，即各次重复之间实测的最小..最大值。
在一台忙碌的 32 核桌面上，中位离差 18–48%，最差的场景（MSVC 的 S1）在同一二进制的不同运行之间
曾波动 245%。
请把加速比当作数量级（"读密集嵌套负载上快一个数量级"，而不是"13.59×"）；漂移请用同一固定 runner 上
`compare-ci-*.md` 的配对报告。

`-Repeats` 提高的是置信度，不是机器的安静程度。测量前把其他东西都关掉，否则测出来的是你的浏览器。

## 遥测

每次运行在原有列之后还会报告：失败的 `try_lock`/`try_lock_shared` 次数、随之发生的
`yield()` 次数，以及进程 CPU 毫秒（user/kernel 分开）。`run_bench.ps1` 会把它们
（多次重复的中位数）汇总成报告里的 **Telemetry** 表。

这些是诊断量，不是 gate。在 S3 上，`recursive_mutex` 基线的失败次数可以比 RSM 高两个数量级，
而 RSM 自己的数字保持平稳；speedup 的摆动来自基线。kernel 占比高说明时间花在调度器里，
而不是锁上。

## 交替对比（`-Compare`）

`run_bench.ps1 -Compare -Rounds N` 让每个场景在**同一个进程**里跑：基线与库交替（ABBA），
每一轮记录 `RSM ops / 基线 ops`，报告取这些比值的中位数，而不是两个独立中位数之比。
它写出 `compare-<toolchain>.md`（已被 git 忽略），含每个场景的中位比值、IQR，以及
"健康轮次"数——健康指基线达到它本轮最好成绩的 80% 以内。

在 `try_lock` + `yield` 自旋下基线是双峰的：它一旦 convoy，比值就被抬高。健康轮次数
说明了这个数字里有多少是基线行为、多少是库本身。这是每周的默认路径：
`mode=compare`（定时任务的默认值）写出 `compare-ci-<toolchain>.md`（提交进仓库，
作为下一次运行的 gate 基线）以及发给 Bencher 的 BMF，同一次测量既供 gate 也供绝对历史。
gate 用健康比值对比上一份报告，健康轮次少于 3 的场景跳过。pull request 的门槛也用这份报告；
`measure` 模式保留给桌面数字和手动检查。

## 场景

| ID | 说明 |
|----|------|
| S1 | 15 读者 / 1 写者，嵌套深度 3 |
| S2 | 2 读者 / 12 写者 |
| S3 | 8 / 8 均衡 |
| S4 | 16 工作线程，约 20% 写 |
| S5 | 16 工作线程，约 5% 写 |
| S6 | 8 分片，16 工作线程，约 15% 写 |
| S7 | 纯独占开销（RSM 不使用 `lock_shared`） |

## 已入库的结果

`results/<toolchain>.md` —— 在真实工作站上测得，是其他文档引用的代表性数字。本地重新生成即可；CI 不会碰它们。

- [results/msvc.md](results/msvc.md)
- [results/mingw-gcc.md](results/mingw-gcc.md)

`results/compare-ci-<toolchain>.md` —— 4 核 GitHub runner，由每周的 compare 运行刷新。它是下一次 gate 的基线；
**不要拿它和上面那两份比较**。

每份报告都记录它测的是哪个后端（`- Backend: ...`），因为 MinGW 的数字在 v0.3.0 改变了含义：以前是 winpthreads 上的
`std::shared_mutex`，现在是原生 SRWLOCK 包装。v0.3.0 之前生成的报告在 MinGW 一栏写的是 `std::shared_mutex`，
与之后的不可比。

- [results/compare-ci-msvc.md](results/compare-ci-msvc.md)
- [results/compare-ci-mingw-gcc.md](results/compare-ci-mingw-gcc.md)
