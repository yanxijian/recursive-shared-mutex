# recursive-shared-mutex

**한국어** · [English](README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md)

<!-- translated-from: README.md sha256:5765763fdfd8 -->
[![CI](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml/badge.svg)](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

C++17 헤더 온리 재진입 가능 `shared_mutex`입니다. 중첩된 호출을 모두 펼치지 않고도 코드베이스를 `std::recursive_mutex`에서 읽기/쓰기 잠금으로 옮길 수 있습니다.

```cpp
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <mutex>
#include <shared_mutex>

rsm::RecursiveSharedMutex mtx;

void write() {
  std::unique_lock lock(mtx);  // 배타적, 재진입 가능
  // ...
}

void read() {
  std::shared_lock lock(mtx);  // 공유, 재진입 가능
  // ...
}
```

## 목차

- [기능](#기능)
- [샤드 배열](#샤드-배열에는-캐시-정렬-버전을-쓴다)
- [컴파일 시 설정](#컴파일-시-설정)
- [std::recursive_mutex와 비교](#stdrecursive_mutex와-비교)
- [요구-사항](#요구-사항)
- [빠른-시작](#빠른-시작)
- [의미론(요약)](#의미론요약)
- [쓰기-기아-관측(R2)](#쓰기-기아-관측r2)
- [제약](#제약)
- [저장소-구조](#저장소-구조)
- [기여](#기여)
- [보안](#보안)
- [라이선스](#라이선스)

## 기능

- `std::unique_lock`과 `std::shared_lock`에서 동작합니다(C++17 *SharedMutex* 표면. 예외가 하나 있으며 [제약](#제약) 참조).
- 재귀는 스레드마다, 뮤텍스마다 셉니다. 카운터는 뮤텍스 주소를 키로 하는 POD 슬롯 표에 있고, 해제 시 슬롯을 재활용하므로 힙 할당이 없습니다.
- 쓰기→읽기 중첩을 허용합니다. 읽기→쓰기 승격은 `std::system_error`를 던집니다.
- `try_lock`, `try_lock_shared`.
- 디버그 단언용 `is_held_exclusive()`. 소유 스레드에서만 참이고 공유 보유자에서는 거짓이며, `!try_lock()`으로는 구분할 수 없습니다.
- 샤드 배열용 `CacheAlignedRecursiveSharedMutex`: 상태 기계는 같고 64바이트 정렬입니다. 여러 뮤텍스가 나란히 있을 때 처리량이 대략 두 배가 됩니다.
- 헤더 온리 CMake `INTERFACE` 타깃: `rsm::recursive_shared_mutex`.

## 샤드 배열에는 캐시 정렬 버전을 쓴다

기본 인스턴스는 24바이트라 64바이트 캐시 라인 하나를 약 2.6개가 공유합니다. 배열이나 vector에서 여러 뮤텍스가 나란히 있으면 샤드끼리 소유자와 카운터 갱신에서 거짓 공유(false sharing)를 일으킵니다. 샤드 8개, 스레드 16개, 쓰기 0 %/15 %/50 %로 MSVC와 MinGW에서 측정한 값:

| | 순수 `RecursiveSharedMutex` | `CacheAlignedRecursiveSharedMutex` |
|---|---:|---:|
| 총 ops/s | 13.9–14.3 M | **31.3–33.2 M**(×2.2–2.4) |

```cpp
std::vector<rsm::CacheAlignedRecursiveSharedMutex> shards(8);
```

정렬된 타입을 따로 둔 이유는 뮤텍스를 객체별 구조체에 넣을 때 인스턴스당 64바이트가 낭비이기 때문입니다. 재현은 `bench_rsm --impl rsm_padded --mode sharded`.

## 컴파일 시 설정

| 매크로 | 기본값 | 효과 |
|---|---|---|
| `RSM_SHARED_SLOT_COUNT` | `256` | 스레드당 추적 가능한 뮤텍스 상한. 슬롯 하나가 TLS 16 B라 기본값은 스레드당 4 KB를 씁니다. 클래스 멤버 배열의 크기를 정하므로 모든 번역 단위에서 같아야 하며, 타깃별로 설정하면 안 됩니다. |
| `RSM_VERSION_MAJOR/MINOR/PATCH`, `RSM_VERSION` | `0/4/1` | 파일만 보는 소비자(FetchContent)를 위한 헤더 버전. `project(VERSION)`과 동기화합니다. |
| `RSM_BACKEND_SRWLOCK` | MinGW에서 켜짐, 그 외 꺼짐 | Win32 SRWLOCK 래퍼를 백엔드로 씁니다. `__MINGW32__`에서는 대안이 건전하지 않아 자동 선택됩니다. |
| `RSM_BACKEND_STD` | 꺼짐 | MinGW에서도 `std::shared_mutex`를 강제합니다. winpthreads 결함을 재현할 때만 유용하며, 주간 CI 프로브가 그 일을 합니다. |
| `RSM_NO_DEBUG_ASSERT` | 꺼짐 | 디버그 전용 "잠근 채 파괴" 단언을 끕니다. |
| `RSM_NO_THREAD_MIGRATION_NOTICE` | 꺼짐 | C++20 코루틴 모드 안내를 끕니다. |

`RSM_SHARED_SLOT_COUNT`는 클래스 정의의 일부이며 사적인 조정 손잡이가 아닙니다. 한 타깃이나 한 소스 파일에서만 설정하면 ODR 위반(IFNDR)입니다. 링커가 한 정의로 합쳐 버리고 다른 번역 단위에는 크기가 다른 표가 남을 수 있습니다. `add_subdirectory()`나 FetchContent로 가져오면 캐시 변수가 링크되는 모든 타깃에 전파됩니다. 설치된 패키지는 빌드 당시 값을 들고 다니므로 소비자는 그 값을 써야 합니다. 헤더를 직접 포함하는 소비자는 매크로를 전역으로 설정해야 합니다.

## `std::recursive_mutex`와 비교

측정 도구는 [benchmarks/README.md](benchmarks/README.md), 툴체인 전반의 그림은 [docs/platform-notes.md](docs/platform-notes.md)를 보세요.

| 배타 전용 경로(S7 형태) | `recursive_mutex` | 이 라이브러리 | 비율 |
|---|---:|---:|---:|
| MSVC(SRWLOCK) | 8.17 M ops/s | 9.53 M ops/s | **1.17×** |
| clang-cl / clang + MSVC STL | 8.07 M ops/s | 11.47 M ops/s | **1.42×** |
| MinGW-GCC 13.2 + `RSM_BACKEND_STD`(libstdc++/winpthreads) | 4.99 M ops/s | 0.34 M ops/s | **0.067×** |
| MinGW-GCC 13.2(기본: 네이티브 SRWLOCK) | 4.99 M ops/s | 7.54 M ops/s | **1.52×** |

> **측정 규약:** 스레드 8개, `try_lock` + `yield` 스핀, 중첩 깊이 3, 32코어 Windows 한 대에서 1초짜리 회차를 5번 교차 측정한 중앙값. 원시 ops/s와 같은 표는 [docs/platform-notes.md](docs/platform-notes.md) §2와 §6에 있습니다.
>
> [benchmarks/results](benchmarks/results)(3초 × 5회, 잠금 연산 사이에 실제 작업)와는 다른 측정 도구라 비율이 다릅니다. 그쪽은 S7 형태에서 2.59×(MSVC), 1.32×(MinGW, 네이티브)를 보고하고, 여기서는 1.17×와 1.52×입니다. 둘 다 배타 경로가 동등 이상임을 보여 주지만 정확한 비율은 측정 규약에 달려 있고 다른 머신으로 옮겨지지 않습니다. 이 호스트의 회차 간 변동은 읽기 위주 시나리오(S1)에서 245 %에 이르므로 수치는 자릿수 수준으로 보세요.

- **MSVC / clang + MSVC STL:** 래퍼 비용은 잠금 한 쌍당 약 1.3 ns입니다(원시 백엔드 3.3 ns, 래퍼 경유 4.6 ns. 둘 다 경합 없음). 비용은 그 스레드가 다른 곳에서 공유 잠금을 몇 개나 들고 있는지에 더 이상 좌우되지 않습니다. 배타 전용 오버헤드는 8스레드 표에서 **1.17×–1.42×**(3초 × 5회 데스크톱 측정에서는 2.59×)이고, 읽기 위주 부하에서는 총 처리량이 대략 **6×–11×** 좋아집니다.
- **MinGW-GCC에서 `RSM_BACKEND_STD`**(예전 기본값): 0.07×–0.2×, 배타 경로에서 5×–15× 후퇴입니다. 원인은 설계가 아니라 툴체인입니다. winpthreads 위의 libstdc++ `std::shared_mutex`는 다른 툴체인의 SRWLOCK 계열 백엔드보다 약 11배 높은 바닥을 가집니다(경합 없음에서 37.4 대 3.3 ns). 래퍼의 TLS 작업까지 더하면 배타 한 쌍이 61.5 대 24.2 ns가 되고, 모든 `thread_local` 접근이 emutls를 거치며, 그 백엔드의 `try_lock()`은 상호 배타가 아닙니다. 그래서 MinGW에서는 네이티브 백엔드가 기본입니다.
- **MinGW-GCC에서 네이티브 백엔드(기본):** 배타 잠금/해제 한 쌍이 62.7 → 24.2 ns(2.6×), 공유 한 쌍이 64.4 → 25.8 ns(2.5×), 배타 전용 비율이 0.067×에서 **1.52×**로 올라 `std::recursive_mutex`보다 빨라집니다. 백엔드 계약 테스트도 다시 통과합니다.

### MinGW-w64: 네이티브 백엔드가 기본

```bash
g++ ...                            # 추가할 것 없음: MinGW는 네이티브 백엔드를 쓴다
g++ -DRSM_BACKEND_STD ...          # std::shared_mutex로 되돌리기(거기서는 건전하지 않음)
```

MinGW-w64에서 헤더는 스스로 `RSM_BACKEND_SRWLOCK`을 고릅니다. 대안이 느릴 뿐 아니라 틀렸기 때문입니다. 이 전환은 `std::shared_mutex`를 8바이트 SRWLOCK 래퍼(`rsm::detail::SrwLockBackend`, `RecursiveSharedMutex::backend_type`으로 노출)로 바꿉니다. MSVC에서는 아무것도 바뀌지 않습니다(거기서 `std::shared_mutex`가 이미 SRWLOCK). Windows Vista 이상이 필요하고 다른 플랫폼에는 영향이 없습니다. SRWLOCK은 배타 모드에서 재진입이 불가능하지만 여기서는 문제가 없습니다. 클래스가 백엔드에 닿기 전에 모든 재귀를 가로채기 때문입니다.

재현은 [benchmarks/run_bench.ps1](benchmarks/run_bench.ps1)(`-Repeats`회의 중앙값 보고). 변동 폭이 왜 중요한지는 [benchmarks/README.md](benchmarks/README.md)를 보세요.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks/run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

## 요구 사항

- C++17 컴파일러(MSVC, GCC, Clang).
- **C++ 예외가 켜져 있어야 합니다.** 헤더는 오용과 슬롯 표 고갈을 `std::system_error`로 알리고, 예외가 꺼져 있으면 `#error` 하나에서 멈춥니다. `cl.exe`, `g++`, `clang++`은 기본으로 예외가 켜져 있지만 **`clang-cl`은 아닙니다**. `/EHsc`를 넘기세요. CMake 타깃이 MSVC 호환 컴파일러에 `/EHsc`를 추가합니다.
- 빌드, 테스트, 설치에 CMake 3.16 이상.

## 빠른 시작

### CMake(권장)

```cmake
add_subdirectory(recursive-shared-mutex) # 또는 FetchContent
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

FetchContent 예:

```cmake
include(FetchContent)
FetchContent_Declare(
  recursive_shared_mutex
  GIT_REPOSITORY https://github.com/yanxijian/recursive-shared-mutex.git
  GIT_TAG v0.4.1
)
FetchContent_MakeAvailable(recursive_shared_mutex)
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

### 빌드와 테스트

```bash
cmake -S . -B build -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## 의미론(요약)

| 상황 | 동작 |
|-----------|----------|
| 같은 스레드가 `lock` / `lock_shared`에 재진입 | 허용 |
| 배타를 보유한 채 `lock_shared` | 허용 |
| 공유를 보유한 채 `lock` | `resource_deadlock_would_occur`를 던짐 |
| 공유를 보유한 채 `try_lock` | `false` 반환 |
| 소유하지 않은 `unlock`, 또는 중첩 공유 잠금이 남은 상태의 `unlock` | `operation_not_permitted`를 던짐 |
| 스레드별 슬롯 표 고갈 | `lock_shared`는 `resource_unavailable_try_again`을 던지고 `try_lock_shared`는 `false` 반환 |
| 다른 OS 스레드에서 해제 | 미지원(TLS) |
| DLL/EXE 경계를 넘는 해제 | 미지원. 두 번째 모듈의 `lock_shared`가 백엔드에 재진입하므로 UB |

배타 재귀는 인스턴스의 원자적 소유자 ID를 씁니다. 공유 재귀는 뮤텍스 주소를 키로 하는 `thread_local` POD 슬롯 표를 쓰며, `[0, used)`에 조밀하게 유지하고 해제 시 재활용하며 스레드 종료 시 절대 해제하지 않습니다.

> **오용은 프로세스를 종료시킵니다. 호출자에게 예외를 던지지 않습니다.** 위의 `unlock` 경우들은 `std::system_error`를 던지지만, 그 호출은 `std::unique_lock` / `std::shared_lock` 소멸자에서도 옵니다. 소멸자는 `noexcept`이므로 짝이 맞지 않으면 `std::terminate`를 부릅니다. 페일패스트이며 잡을 수 없습니다. 헤더 주석을 보세요.

이 컴포넌트의 정확성은 밑에 있는 `std::shared_mutex`의 정확성을 넘지 못합니다. [tests/backend_contract.cpp](tests/backend_contract.cpp)가 현재 툴체인에서 그 계약을 검증합니다. 새 컴파일러나 표준 라이브러리를 믿기 전에 실행하세요.

마이그레이션 안내와 테스트 조합은 [docs/migration.md](docs/migration.md)를 보세요.

## 쓰기 기아 관측(R2)

쓰기를 우선하지 않는 백엔드는 읽기 부하가 이어지면 쓰기를 굶깁니다. glibc의 `pthread_rwlock`과 Win32 SRWLOCK이 그 예입니다. 선택적으로 쓰는 계측 가드는 핵심 컴포넌트를 건드리지 않고 쓰기 대기 시간과 보유 시간을 측정합니다:

```cpp
#include <recursive_shared_mutex/instrumented_write_lock.hpp>

// 시작 시 한 번 설치(모든 가드보다 오래 살아야 함):
rsm::metrics::AtomicStatsRecorder stats;
rsm::metrics::set_recorder(&stats);

// 핫 패스:
{
    rsm::metrics::instrumented_write_lock guard(mtx); // 대기 + 보유 기록
    // ... 배타 작업 ...
}
// stats.snapshot() -> rsm_write_wait_seconds / rsm_write_hold_seconds로 흘려보내기
```

기록기를 설치하지 않으면 가드 비용은 0입니다. 지표와 알림 계약은 `docs/roadmap.md`의 R2를 보세요.

## 제약

- TLS 슬롯 표는 모듈마다 다릅니다. DLL이나 EXE 경계가 관여하면 한 뮤텍스의 `lock_shared` / `unlock_shared` 순서를 단일 모듈의 TLS 영역 안에 두거나, TLS 상태를 하나의 공개 모듈에 두세요. 이를 어기면 슬롯만 남는 것이 아니라 정의되지 않은 동작입니다. [examples/cross_module_hazard](examples/cross_module_hazard)가 실제 DLL/.so로 재현합니다.
- 현재 스레드가 공유 잠금을 보유한 채 뮤텍스를 파괴하면 정의되지 않은 동작입니다. 디버그 빌드는 단언합니다. `NDEBUG`나 `RSM_NO_DEBUG_ASSERT`에서는 소멸자가 피해를 줄이려고 그 스레드의 슬롯을 조용히 버리지만, 파괴가 안전해지는 것은 아닙니다.
- 한 스레드가 동시에 보유할 수 있는 공유 잠금은 `kSharedSlotCount`(256)까지이며 스레드당 약 4 KB의 TLS입니다. 다음 `lock_shared`는 `resource_unavailable_try_again`을 던지고 `try_lock_shared`는 `false`를 반환합니다. 슬롯은 해제 시 재활용되므로 여러 뮤텍스를 차례로 만지는 것은 문제없습니다.
- 공유에서 배타로의 승격은 없습니다. `lock()`은 `resource_deadlock_would_occur`를 던집니다. 읽는 쪽을 `shared_lock`으로 옮기기 전에 읽기→쓰기 호출 사슬을 점검하세요.
- 밑의 `std::shared_mutex`는 쓰기를 우선하지 않으므로 읽기가 계속되면 쓰기가 굶을 수 있습니다(벤치마크 S1/S5 참조).
- `std::condition_variable_any`는 재귀 깊이가 1을 넘으면 안전하지 않습니다. `wait()`는 한 단계만 풀기 때문에 뮤텍스는 잡힌 채로 남고 대기자는 영원히 깨어나지 않습니다.
- 백엔드는 상호 배타를 지켜야 합니다. MinGW-w64 GCC 13.2(libstdc++/winpthreads)는 지키지 않습니다. `try_lock()`이 소유자 둘을 인정해 버려 `unlock()`에서 엉뚱한 `operation_not_permitted`가 나옵니다. MinGW에서 네이티브 백엔드를 기본으로 하고 `RSM_BACKEND_STD`를 선택 사항으로 둔 이유입니다. 새 툴체인에서는 반드시 [tests/backend_contract.cpp](tests/backend_contract.cpp)를 실행하세요. 자세한 내용은 [docs/platform-notes.md](docs/platform-notes.md).
- **문자 그대로의 `Cpp17Lockable`은 아닙니다.** `unlock()`과 `unlock_shared()`는 오용 시 `std::system_error`를 던지지만, 이름 있는 요구 사항은 *Throws: Nothing*(`[thread.mutex.requirements.mutex]`)입니다. 일반 알고리즘은 자기가 획득한 것만 풀기 때문에 `std::lock`, `std::scoped_lock`, 두 RAII 래퍼는 동작합니다. 소멸자에서 도달한 짝이 맞지 않는 조합은 위에서 말한 대로 `std::terminate`를 부릅니다.
- **MinGW는 `<windows.h>`를 끌어옵니다.** 기본 네이티브 백엔드가 이것을 포함하며 컴파일 시간이 실제로 늘어납니다. 이 수치가 나온 머신에서 번역 단위 하나당 655 ms 대 207 ms(3.2×). `RSM_BACKEND_STD`는 포함을 피하지만 그 툴체인에서 건전하지 않아 깔끔하게 뺄 방법이 없습니다.

## 저장소 구조

```text
include/recursive_shared_mutex/   # 공개 헤더
tests/                            # Catch2 테스트 + 프레임워크 비의존 회귀 테스트
examples/                         # 사용 예(모듈 경계 위험 재현 포함)
benchmarks/                       # 드라이버, CI 자동화, 결과 기록
docs/                             # 설계, 마이그레이션, 플랫폼 메모, 로드맵, 검증
verification/                     # 소크 이력, 기록한 배치마다 한 줄
scripts/repo_rules.py             # AGENTS.md가 CI로 지킨다고 약속한 규칙(RepoRules 내장본)
cmake/                            # 패키지 설정
.github/                          # 워크플로, 이슈/PR 템플릿, CONTRIBUTING / SECURITY / CODE_OF_CONDUCT
AGENTS.md                         # 저장소 규칙. 모든 에이전트에게 하나의 파일
```

## 기여

[CONTRIBUTING.md](.github/CONTRIBUTING.md)를 보세요. 큰 설계 변경 전에는 이슈를 열어 주세요.

저장소 규칙은 [AGENTS.md](AGENTS.md)에 있고 Claude Code, Codex, Cursor, Copilot 등 모두에게 하나의 파일입니다. `CLAUDE.md`, `.cursor/rules/repo.mdc`, `.github/copilot-instructions.md`는 그곳을 가리킵니다. 기계적 규칙(UTF-8 BOM과 CRLF, 140열, `#pragma comment(lib)` 금지, 헤더 위생, 버전 동기화, CHANGELOG 절, 번역문이 계속 링크되어 있을 것)은 push마다 CI에서 RepoRules(`scripts/repo_rules.py`)가 검사하므로 리뷰를 기다리지 않고 빌드가 실패합니다. 기계적인 것은 `python3 scripts/repo_rules.py fix --config repo-rules.json`으로 고치고, 커밋 전에 검사하려면 `git config core.hooksPath .githooks`를 실행하세요.

## 보안

[SECURITY.md](.github/SECURITY.md)를 보세요.

## 라이선스

[MIT](LICENSE) © 2026 yanxijian
