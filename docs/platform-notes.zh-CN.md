# 平台实测记录：各工具链上的实际行为

[English](platform-notes.md) · **简体中文**

<!-- translated-from: docs/platform-notes.md sha256:2378fc0461aa -->

所有数字都在同一台 Windows x64 主机上测得（争用场景 8 线程，`-O2`/`/O2`，非争用场景 2 M 次迭代）。
绝对值与机器强相关；重要的是**比值**和通过/失败结果。`S7` 的比值取 5 轮交错 1 秒测量的中位数，
因为这台主机上单次测量会摆动 1.1×–3.2×。

## 1. 非争用开销（每次 lock/unlock 配对的纳秒数）

| 实现 | MinGW-GCC 13.2 (UCRT) | MSVC 19.51 | clang 22.1 (MSVC STL) |
|---|---:|---:|---:|
| `rsm::RecursiveSharedMutex` 独占 | 61.5 | 5.8 | 5.9 |
| `rsm::RecursiveSharedMutex` 共享 | 73.7 | 7.2 | 7.4 |
| 嵌套独占（×2） | 71.4 | 9.9 | 10.6 |
| 嵌套共享（×2） | 105.2 | 12.0 | 11.2 |
| `std::recursive_mutex` | 12.5 | 4.7 | 4.7 |
| `std::shared_mutex`（后端下限） | 37.4 | 3.3 | 3.2 |
| `sizeof(std::thread::id)` | 8 | 4 | 4 |
| `sizeof(rsm::RecursiveSharedMutex)` | 24 | 24 | 24 |
| `sizeof(std::recursive_mutex)` | 8 | 80 | 80 |

测于 v0.2.x。从 v0.3.0 起，MinGW 那一列描述的是 **`RSM_BACKEND_STD`** 配置（MinGW 现在默认原生后端，
其非争用成本见第 6 节）；MSVC 的独占行在独占快路径不再扫描槽位表之后是 **4.6 ns**（共享 6.8 ns）。

解读：包装成本约为其底层后端的 1.7×，但**绝对**成本主要由后端决定（MinGW 上慢 11×）和 TLS 访问决定
（MinGW 的 emutls 比 MSVC/clang 用的 `__declspec(thread)`/TLS 描述符路径贵一个数量级）。在 MSVC/clang 上，
整个组件与 `std::recursive_mutex` 的差距在 25% 以内；在 MinGW 上是慢 5×。注意非争用时共享路径**比**独占路径更贵——
TLS 槽位查找要靠真正的读者并发才能回本。

## 2. 纯独占路径（跑分里的 S7 形态）

8 线程，`try_lock` + `yield` 自旋，嵌套深度 3，5 轮交错 1 秒测量的中位数：

| 工具链 | `recursive_mutex` | 本库 | 比值 |
|---|---:|---:|---:|
| MSVC 19.51 | 8,167,734 ops/s | 9,532,459 ops/s | **1.17×** |
| clang 22.1 (MSVC STL) | 8,068,846 ops/s | 11,468,567 ops/s | **1.42×** |
| MinGW-GCC 13.2 | 4,986,027 ops/s | 335,929 ops/s | **0.067×** |

MinGW 那一行是 `RSM_BACKEND_STD`；原生后端在第 6 节达到 1.52×。已入库的跑分表用的是另一套框架
（3 秒 × 5 次、加解锁之间有实际工作），S7 形态报 2.59×（MSVC）与 1.32×（MinGW 原生后端）：
两者不能直接相比，具体比值取决于协议。那次 MinGW 运行还在五轮中出现过两次虚假的 `operation_not_permitted`，正是第 3 节描述的症状。

## 3. 后端契约（为什么 MinGW 不只是慢）

`std::shared_mutex` 只保证"和它的实现一样好"。在 MinGW-w64 GCC 13.2 / clang 配 libstdc++ 走 **winpthreads**
时——也就是 `RSM_BACKEND_STD`，因为 MinGW 的默认是原生后端——`try_lock()` 并不互斥：在 `try_lock`/`unlock`
风暴下，两个线程可以同时持有写锁。8 线程、临界区约 200 ns 的风暴：

| 工具链 | 两个独占属主 | 写者在内时读者也在内 |
|---|---:|---:|
| MSVC 19.51 | 0 / 134,371 | 0 / 294,219 |
| clang 22.1 (MSVC STL) | 0 / 200,927 | 0 / 323,282 |
| MinGW-GCC 13.2 | **3–171 / ~140,000** | **268–502 / ~500,000** |
| clang 22.1 + libstdc++/winpthreads | **135 / 140,080** | **268 / 489,375** |

缺陷跟着运行时走，不跟编译器前端走。对本库的后果：

- `try_lock()` 可能对一个并不拥有该 mutex 的线程返回 `true`，于是 `owner_` / `exclusive_count_` 被"第二个属主"
  覆盖，真正属主的 `unlock()` 抛 `operation_not_permitted`（约每 50,000 次成功获取 1 次）。这是继承来的缺陷，
  不是包装层造成的。
- 读者靠 `try_lock`/`try_lock_shared` 自旋的跑分（`benchmarks/bench_vs_recursive_mutex.cpp` 全部如此）在该工具链上
  测的是一个已经损坏的原语，因此 MinGW 那张表不能当作设计结论。

`tests/backend_contract.cpp` 用一个无依赖的文件检查这一点，它也是 CI 矩阵里的第一个 job。注意"一个线程持锁、
另一个线程 `try_lock` 探测"这种稳态测试**抓不到**它——窗口在 `unlock`/`try_lock` 的交错处。

## 4. 其它已测配置

| 配置 | 后端契约 | 库级互斥 |
|---|---|---|
| MSVC x64 Release / Debug | 通过 | 通过 |
| MSVC x86（32 位）Release | 通过 | 通过 |
| clang-cl x64 Release / Debug | 通过 | 通过 |
| clang x64 Release (MSVC STL) | 通过 | 通过 |
| MinGW-GCC 默认（原生 SRWLOCK） | 通过 | 通过 |
| MinGW-GCC `RSM_BACKEND_STD` | **失败**（原语后端） | 通过（阻塞路径占主导） |
| clang + libstdc++/winpthreads（与 `RSM_BACKEND_STD` 同后端） | **失败** | 通过 |

CI 如何看住这件事：`.github/workflows/platform-matrix.yml` 每周跑一个 job
`expected-fail / mingw winpthreads (red = upstream fixed)`，它用 `RSM_BACKEND_STD` 构建，并**期望**契约被违反。
缺陷存在时它是绿的，只有当工具链不再违反契约时才变红；那说明可以删掉它了。

32 位细节：`sizeof(rsm::RecursiveSharedMutex)` 降到 16，`lock+unlock` 为 7.9 ns，`std::recursive_mutex` 为 9.7 ns
——在那个配置上，包装层比它替换掉的东西更快。

本文未覆盖（只能在 Linux/macOS CI 上触达）：glibc 与 musl 的 rwlock 语义、libc++ 的 `shared_mutex`、
ThreadSanitizer、弱内存序硬件。`.github/workflows/platform-matrix.yml` 覆盖了这些。

## 5. 如何复现

```bash
# 后端契约，任意工具链，不需要构建系统
c++ -O2 -std=c++17 -pthread -Iinclude tests/backend_contract.cpp -o bc && ./bc
cl /O2 /std:c++17 /EHsc /Iinclude tests\backend_contract.cpp
g++ -O2 -std=c++17 -pthread -Iinclude tests/backend_contract.cpp -o bc.exe && ./bc.exe

# 库级互斥
c++ -O2 -std=c++17 -pthread -Iinclude tests/mutual_exclusion.cpp -o me && ./me

# 完整套件
cmake -S . -B build -DRSM_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

# 跨模块 TLS 域（真实 DLL/.so；预期会显示 "separate TLS domain"）
cd examples/cross_module_hazard && ./build_and_run.sh     # Linux/macOS
build_and_run.cmd msvc                                     # Windows
```

## 6. SRWLOCK 后端（MinGW 上的默认）

`RSM_BACKEND_SRWLOCK` 在同一个类里用 8 字节的 SRWLOCK 包装替换 `std::shared_mutex`。自引入 `RSM_BACKEND_STD`
的那个版本起，MinGW-w64 会自动选择它：那里的另一个选项不只是慢，而是不可靠。`-DRSM_BACKEND_STD` 可以恢复
`std::shared_mutex`。在 MinGW-w64 GCC 13.2 上测得，二进制布局相同、测试相同：

| | `RSM_BACKEND_STD` (`std::shared_mutex`) | `RSM_BACKEND_SRWLOCK`（默认） |
|---|---:|---:|
| `lock+unlock` | 62.7 ns/配对 | **24.2 ns/配对**（2.6×） |
| `lock_shared+unlock_shared` | 64.4 ns/配对 | **25.8 ns/配对**（2.5×） |
| 2× 嵌套独占 | 72.5 ns | **33.9 ns** |
| 2× 嵌套共享 | 86.7 ns | **47.2 ns** |
| S7 形态 vs `std::recursive_mutex` | 0.068× | **1.523×** |
| `backend_contract` | **失败** | **通过** |
| 完整套件 | **失败** | **通过** |

共享相关的两行在 v0.4.0 上重测：`is_held_exclusive()` 现在会在没有独占属主时跳过 `pthread_self()` 调用，
两种后端都受益。独占与 S7 两行不受该改动影响，仍是更早的测量。

在 MSVC 上这个开关实际上是空操作（`std::shared_mutex` 本来就是 SRWLOCK）：5.8 vs 5.7 ns，两种方式契约都通过。

它**没有**修掉的是：MinGW 上剩下的约 20 ns 包装开销，是槽位表的 TLS/emutls 成本（`sizeof(RecursiveSharedMutex)`
在两种后端上都是 24 B）。要消掉它需要"实例里只放后端"的重构——原型里 std 后端总共约 40 ns、SRWLOCK 约 4 ns，
也就是说那是该平台上的下一个 3×，不是当前这一个。

## 7. 试过并否决的方案（附数字）

另外两个优化在原型阶段被测量后放弃。记录下来是为了没人重复这项工作，也因为负面结果能说明成本到底在哪。
本节基线来自 v0.3.x；v0.4.0 降低了共享路径（第 6 节），所以下面的比值是历史值。

### 纯 TLS 状态（实例里只放后端）

把 `owner_` / `exclusive_count_` / `under_exclusive_` 搬进槽位表，让对象只有 8 字节、且永不读取线程 id。
同样一套测试在 MSVC、clang 和 MinGW + SRWLOCK 上全部通过：

| | 当前（混合） | 纯 TLS |
|---|---:|---:|
| MinGW + SRWLOCK：独占 | 23.4 ns | 27.5 ns |
| MinGW + SRWLOCK：2× 嵌套独占 | 32.9 ns | **52.0 ns** |
| MinGW + SRWLOCK：共享 | 35.1 ns | **15.7 ns** |
| MinGW + SRWLOCK：2× 嵌套共享 | 66.1 ns | **28.6 ns** |
| MSVC：独占 / 共享 | 5.8 / 7.2 ns | 5.9 / 6.1 ns |
| `sizeof` | 24 B | **8 B** |

它在 MinGW 上输在哪里：当前设计让**递归独占**快路径完全不碰 TLS（就是两个实例内的原子变量），而纯 TLS 设计在每次 `lock()` **和**每次 `unlock()` 上都要付一次槽位查找。既然独占路径正是 MinGW 需要帮助的那条，这个重构等于拿"有问题的平台"去换"本来就没问题的平台"。否决。

### 单条槽位提示 + 每次操作只取一次 TLS

`findActiveSlot()` 取了两次线程局部表，共享路径每次操作取三次；一个经过校验的单条提示加每调用点一次取值，
本应消除这些。在 MinGW 上实测：

| | 改前 | 改后 |
|---|---:|---:|
| 独占 `lock+unlock` | 62.8 ns | 61.7 ns |
| 共享 `lock_shared+unlock_shared` | 76.4 ns | 74.4 ns |
| 独占，`RSM_BACKEND_SRWLOCK` | 23.4 ns | 23.2 ns |
| 共享，`RSM_BACKEND_SRWLOCK` | 35.1 ns | 36.1 ns |

全在噪声范围内。结论：在这条工具链上，每次操作的线程局部取值**不是**成本中心（`thread_local` int 写入实测约
0.4 ns），所以额外的表状态没有保留。

### 剩下的是什么

用 `RSM_BACKEND_SRWLOCK` 时，包装层比裸 SRWLOCK 每对多约 20 ns（23.2 vs 3.4）。它分散在
`std::this_thread::get_id()`（winpthreads 的 `pthread_self`）、`owner_` 上的两个 relaxed 原子、槽位记账和后端调用上
——没有任何单点微改能挪动它。下一步应该是在目标工具链上跑 profiler，而不是继续猜；`benchmarks/` 目前无法区分这些贡献。
