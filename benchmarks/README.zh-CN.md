# 与 `std::recursive_mutex` 的跑分对比

[English](README.md) · **简体中文**

<!-- translated-from: README.md sha256:bc6b85bc5cd1 -->

在嵌套加锁、约 4 MiB 工作集的条件下，对比 `rsm::RecursiveSharedMutex`（除特别说明外读者走共享锁）与
`std::recursive_mutex`。

## 运行

```powershell
# 可移植：只需 PATH 上有 cl.exe 或 g++，不硬编码 Visual Studio 路径。
#   -Toolchain msvc|mingw|both   -Seconds 3   -Repeats 3   -OutDir results
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

每个 (场景, 实现) 组合都在**独立进程**中运行，这样 MinGW `shared_mutex` 的压力测试不会污染同进程后续的配置。
每个组合跑 `-Repeats` 次并报告**中位数**：在共享机器上单次运行噪声太大，无法用于比较。

跑分驱动通过 CMake 构建（`-DRSM_BUILD_BENCHMARKS=ON`，目标 `bench_rsm`），因此不依赖 Visual Studio 装在哪里。

## 自动化

[`.github/workflows/benchmarks.yml`](../.github/workflows/benchmarks.yml) 在固定的 `windows-2022` runner 上跑这些场景：

- **每周（周一 03:00 UTC）**及手动触发：测量 MSVC 与 MinGW，并**自行把刷新后的 `results/ci-*.md` 提交到 `main`**
  （默认 `update_mode: auto-merge`；`pull-request` 会开 PR 留待评审，`commit` 直接提交到工作流所在分支，
  `none` 只测量）。除非每个改动路径都匹配 `benchmarks/results/ci-*.md`，否则拒绝提交，因此流水线碰不到代码，
  也碰不到桌面机器的数字。
- **改动 `include/**` 的 PR**（是库本身，不是跑分脚本或这些文档）：短跑一遍并与已提交的基线比较。
  **只有 S6（分片）和 S7（纯独占）会卡住 job**，阈值 40%。在 4 核 runner 上，"读并行"类场景按构造就贴着 1.0×
  ——核数不足以让读者重叠——所以那里 ±25% 的波动是调度噪声而不是回归（实测：S3 从 1.01× 到 0.76×，
  而其他场景都在 ±6% 内）。S6/S7 衡量的是本库自身的包装成本，真正的回归会体现在那里。

它**从不合并 pull request**：既不合它自己开的 PR（那个模式是 `pull-request`，会原样留着），更不合别人的。
唯一的自动合并是 job 自己在几秒前创建的结果分支，且必须每个改动路径都匹配 `benchmarks/results/ci-*.md`。
人类 PR 只被 gate job *检查*，而该 job 是 `permissions: contents: read`，物理上没有合并权限。

刻意的限制与注意事项：

- 绝对 ops/s 属于产出它的那台机器。真正该看的是**同一次运行内**相对 `recursive_mutex` 的加速比。
- 闸门只和 CI 自己产出的基线比较（文件里写着 `Runner: GitHub Actions`）。手工测的文件在不同硬件上，
  因此在工作流刷新过一次之前，闸门只告警而不失败。
- **GitHub runner 只有 4 核。** 共享锁的收益来自"读者数 > 核数"，所以 CI 上 S1–S5 的加速比会塌到接近 1.0
  （实测 0.8–1.06×），而 32 核桌面能到 13×。CI 数字用于同一硬件上的逐次回归检测，不是绝对结论；
  `run_bench.ps1` 在检测到少于 8 核时会在报告里自动注明。
- 自动合并不需要任何仓库设置，因为根本不涉及 PR。只有 `update_mode: pull-request` 需要打开
  Settings > Actions > General > *Allow GitHub Actions to create and approve pull requests*；
  该设置为关时 job 只告警并打印比较链接。

在大机器上让这些指标有意义之前，先本地跑：

```powershell
# 例如在 32 核桌面上
powershell -NoProfile -File benchmarks/run_bench.ps1 -Toolchain msvc -Seconds 3 -Repeats 3
```

## 这些数字有多准？

不太准，报告自己也这么说：每份报告都带一行 `Run-to-run spread`，即各次重复之间实测的最小..最大值。
在一台忙碌的 32 核桌面上，中位离差约 40%，balanced 场景在同一二进制的不同运行之间曾波动超过 250%。
请把加速比当作数量级——"读密集嵌套负载上快一个数量级"，而不是"13.59×"——漂移请用同一固定 runner 产出的
`ci-*.md`。

`-Repeats` 提高的是置信度，不是机器的安静程度：测量前把其他东西都关掉，否则测出来的是你的浏览器。

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

`results/ci-<toolchain>.md` —— 4 核 GitHub runner，由每周工作流刷新。保留它只为在同一硬件上检测并卡住漂移；
**不要拿它和上面那两份比较**。

每份报告都记录它测的是哪个后端（`- Backend: ...`），因为 MinGW 的数字在 v0.3.0 改变了含义：以前是 winpthreads 上的
`std::shared_mutex`，现在是原生 SRWLOCK 包装。v0.3.0 之前生成的报告在 MinGW 一栏写的是 `std::shared_mutex`，
与之后的不可比。

- [results/ci-msvc.md](results/ci-msvc.md)
- [results/ci-mingw-gcc.md](results/ci-mingw-gcc.md)
