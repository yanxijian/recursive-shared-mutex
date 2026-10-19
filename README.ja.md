# recursive-shared-mutex

**日本語** · [English](README.md) · [简体中文](README.zh-CN.md) · [한국어](README.ko.md)

<!-- translated-from: README.md sha256:5765763fdfd8 -->
[![CI](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml/badge.svg)](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

C++17 のヘッダーオンリーな再入可能 `shared_mutex` です。入れ子になった呼び出しをすべて平坦化しなくても、コードベースを `std::recursive_mutex` から読み書きロックへ移せます。

```cpp
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <mutex>
#include <shared_mutex>

rsm::RecursiveSharedMutex mtx;

void write() {
  std::unique_lock lock(mtx);  // 排他・再入可能
  // ...
}

void read() {
  std::shared_lock lock(mtx);  // 共有・再入可能
  // ...
}
```

## 目次

- [特長](#特長)
- [シャード配列](#シャード配列にはキャッシュ境界版を使う)
- [コンパイル時の設定](#コンパイル時の設定)
- [std::recursive_mutex との比較](#stdrecursive_mutex-との比較)
- [動作要件](#動作要件)
- [クイックスタート](#クイックスタート)
- [意味論（要約）](#意味論要約)
- [書き手の飢餓の観測（R2）](#書き手の飢餓の観測r2)
- [制約](#制約)
- [リポジトリ構成](#リポジトリ構成)
- [コントリビュート](#コントリビュート)
- [セキュリティ](#セキュリティ)
- [ライセンス](#ライセンス)

## 特長

- `std::unique_lock` と `std::shared_lock` で使えます（C++17 の *SharedMutex* の面。例外が 1 つあり、[制約](#制約) を参照）。
- 再帰はスレッドごと・ミューテックスごとに数えます。カウンタはミューテックスのアドレスをキーにした POD スロット表にあり、解放時にスロットを再利用するのでヒープ確保はありません。
- 書き→読みの入れ子を許します。読み→書きの昇格は `std::system_error` を投げます。
- `try_lock` と `try_lock_shared`。
- デバッグ用のアサーションに `is_held_exclusive()`。所有スレッドにだけ真になり、共有保持では偽になります。`!try_lock()` ではこの区別ができません。
- シャード配列向けの `CacheAlignedRecursiveSharedMutex`。状態機械は同じで、64 バイト境界に揃えます。複数のミューテックスが隣り合う場合、おおむね 2 倍のスループットになります。
- ヘッダーオンリーの CMake `INTERFACE` ターゲット：`rsm::recursive_shared_mutex`。

## シャード配列にはキャッシュ境界版を使う

既定のインスタンスは 24 バイトなので、64 バイトのキャッシュライン 1 本を約 2.6 個で共有します。配列や vector で複数のミューテックスが隣り合うと、シャード同士が所有者とカウンタの更新でフォルスシェアリングを起こします。8 シャード・16 スレッド・書き込み 0 %／15 %／50 % で、MSVC と MinGW で測った結果：

| | 素の `RecursiveSharedMutex` | `CacheAlignedRecursiveSharedMutex` |
|---|---:|---:|
| 合計 ops/s | 13.9–14.3 M | **31.3–33.2 M**（×2.2–2.4） |

```cpp
std::vector<rsm::CacheAlignedRecursiveSharedMutex> shards(8);
```

揃えた型を別にしているのは、ミューテックスをオブジェクトごとの構造体に埋め込む場合、1 個 64 バイトは無駄だからです。再現は `bench_rsm --impl rsm_padded --mode sharded`。

## コンパイル時の設定

| マクロ | 既定値 | 効果 |
|---|---|---|
| `RSM_SHARED_SLOT_COUNT` | `256` | スレッドごとに追跡できるミューテックス数の上限。スロット 1 つは TLS 16 B なので、既定では 1 スレッドあたり 4 KB を使います。クラスメンバ配列の大きさを決めるため、すべての翻訳単位で同じ値でなければなりません。ターゲットごとに設定しないこと。 |
| `RSM_VERSION_MAJOR/MINOR/PATCH`、`RSM_VERSION` | `0/4/1` | ファイルだけを見る利用者（FetchContent）向けのヘッダーバージョン。`project(VERSION)` と同期させます。 |
| `RSM_BACKEND_SRWLOCK` | MinGW で有効、それ以外は無効 | Win32 の SRWLOCK ラッパーをバックエンドに使います。`__MINGW32__` では代替が不健全なので自動で選ばれます。 |
| `RSM_BACKEND_STD` | 無効 | MinGW でも `std::shared_mutex` を強制します。winpthreads の欠陥を再現するときだけ有用で、週次の CI プローブがそれを行います。 |
| `RSM_NO_DEBUG_ASSERT` | 無効 | デバッグ専用の「ロックしたまま破棄」アサーションを無効にします。 |
| `RSM_NO_THREAD_MIGRATION_NOTICE` | 無効 | C++20 コルーチンモードの注意書きを黙らせます。 |

`RSM_SHARED_SLOT_COUNT` はクラス定義の一部であり、私的な調整つまみではありません。1 つのターゲットや 1 つのソースファイルだけで設定すると ODR 違反（IFNDR）になります。リンカが片方の定義に統合し、他の翻訳単位には大きさの違う表が残るかもしれません。`add_subdirectory()` や FetchContent で取り込む場合、キャッシュ変数がリンクするすべてのターゲットに伝わります。インストール済みパッケージはビルド時の値を持ち運ぶので、利用者はその値を使う必要があります。ヘッダーを直接インクルードする利用者は、マクロを全体で設定してください。

## `std::recursive_mutex` との比較

測定器は [benchmarks/README.md](benchmarks/README.md)、ツールチェーン間の全体像は [docs/platform-notes.md](docs/platform-notes.md) を参照してください。

| 排他専用の経路（S7 の形） | `recursive_mutex` | このライブラリ | 比 |
|---|---:|---:|---:|
| MSVC（SRWLOCK） | 8.17 M ops/s | 9.53 M ops/s | **1.17×** |
| clang-cl / clang + MSVC STL | 8.07 M ops/s | 11.47 M ops/s | **1.42×** |
| MinGW-GCC 13.2 + `RSM_BACKEND_STD`（libstdc++/winpthreads） | 4.99 M ops/s | 0.34 M ops/s | **0.067×** |
| MinGW-GCC 13.2（既定：ネイティブ SRWLOCK） | 4.99 M ops/s | 7.54 M ops/s | **1.52×** |

> **測定手順：** 8 スレッド、`try_lock` + `yield` のスピン、入れ子の深さ 3、32 コアの Windows 1 台で 1 秒の回を 5 回交互に測った中央値。生の ops/s と同じ表は [docs/platform-notes.md](docs/platform-notes.md) の §2 と §6 にあります。
>
> これは [benchmarks/results](benchmarks/results)（3 秒 × 5 回、ロック操作の間に実処理あり）とは別の測定器なので、比が食い違います。あちらは S7 の形で 2.59×（MSVC）と 1.32×（MinGW、ネイティブ）を報告しており、こちらは 1.17× と 1.52× です。どちらも排他の経路が同等以上であることを示しますが、正確な比は測定手順に依存し、別のマシンには移せません。このホストの回ごとのばらつきは読み中心のシナリオ（S1）で 245 % に達するので、数値は桁の目安として扱ってください。

- **MSVC / clang + MSVC STL：** ラッパーのコストはロックの 1 組あたり約 1.3 ns です（生のバックエンドが 3.3 ns、ラッパー経由が 4.6 ns。いずれも競合なし）。コストはそのスレッドが他所で共有ロックをいくつ持っているかに依存しなくなりました。排他専用のオーバーヘッドは 8 スレッドの表で **1.17×–1.42×**（3 秒 × 5 回のデスクトップ測定では 2.59×）、読み中心の負荷では合計スループットがおおむね **6×–11×** 向上します。
- **MinGW-GCC で `RSM_BACKEND_STD`**（以前の既定）：0.07×–0.2×、排他の経路で 5×–15× の後退です。原因は設計ではなくツールチェーンにあります。winpthreads 上の libstdc++ の `std::shared_mutex` は、他のツールチェーンの SRWLOCK 系バックエンドより約 11 倍高い床を持ちます（競合なしで 37.4 対 3.3 ns）。ラッパーの TLS 処理も加わると排他の 1 組は 61.5 対 24.2 ns となり、`thread_local` へのアクセスはすべて emutls を経由し、そのバックエンドの `try_lock()` は相互排他になっていません。そのため MinGW ではネイティブのバックエンドが既定です。
- **MinGW-GCC でネイティブのバックエンド（既定）：** 排他のロック／アンロック 1 組が 62.7 → 24.2 ns（2.6×）、共有の 1 組が 64.4 → 25.8 ns（2.5×）、排他専用の比は 0.067× から **1.52×** になり `std::recursive_mutex` より速くなります。バックエンドの契約テストも再び通ります。

### MinGW-w64：ネイティブのバックエンドが既定

```bash
g++ ...                            # 追加は不要：MinGW はネイティブのバックエンドになる
g++ -DRSM_BACKEND_STD ...          # std::shared_mutex に戻す（そこでは不健全）
```

MinGW-w64 ではヘッダーが自分で `RSM_BACKEND_SRWLOCK` を選びます。代替は遅いだけでなく誤っているからです。この切り替えは `std::shared_mutex` を 8 バイトの SRWLOCK ラッパー（`rsm::detail::SrwLockBackend`、`RecursiveSharedMutex::backend_type` として公開）に置き換えます。MSVC では何も変わりません（そこでは `std::shared_mutex` がすでに SRWLOCK です）。Windows Vista 以降が必要で、他のプラットフォームでは効果がありません。SRWLOCK は排他モードで再入不可ですが、ここでは問題ありません。クラスがバックエンドに触れる前にすべての再帰を横取りするからです。

再現は [benchmarks/run_bench.ps1](benchmarks/run_bench.ps1)（`-Repeats` 回の中央値を報告）。ばらつきがなぜ重要かは [benchmarks/README.md](benchmarks/README.md) を参照してください。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks/run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

## 動作要件

- C++17 のコンパイラ（MSVC、GCC、Clang）。
- **C++ 例外が有効であること。** ヘッダーは誤用とスロット表の枯渇を `std::system_error` で報告し、例外が無効なら 1 つの `#error` で止まります。`cl.exe`、`g++`、`clang++` は既定で例外が有効ですが、**`clang-cl` は違う**ので `/EHsc` を渡してください。CMake ターゲットは MSVC 互換のコンパイラに `/EHsc` を追加します。
- ビルド、テスト、インストールには CMake 3.16 以降。

## クイックスタート

### CMake（推奨）

```cmake
add_subdirectory(recursive-shared-mutex) # または FetchContent
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

FetchContent の例：

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

### ビルドとテスト

```bash
cmake -S . -B build -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## 意味論（要約）

| 状況 | 挙動 |
|-----------|----------|
| 同じスレッドが `lock` / `lock_shared` に再入 | 許可 |
| 排他を保持したまま `lock_shared` | 許可 |
| 共有を保持したまま `lock` | `resource_deadlock_would_occur` を投げる |
| 共有を保持したまま `try_lock` | `false` を返す |
| 所有していない `unlock`、または入れ子の共有ロックが残っている状態での `unlock` | `operation_not_permitted` を投げる |
| スレッドごとのスロット表が枯渇 | `lock_shared` は `resource_unavailable_try_again` を投げ、`try_lock_shared` は `false` を返す |
| 別の OS スレッドでの解放 | 非対応（TLS） |
| DLL/EXE 境界をまたぐ解放 | 非対応。2 つ目のモジュールの `lock_shared` はバックエンドへ再入することになり、未定義動作 |

排他の再帰はインスタンス上のアトミックな所有者 ID を使います。共有の再帰はミューテックスのアドレスをキーにした `thread_local` の POD スロット表を使い、`[0, used)` に密に保ち、解放時に再利用し、スレッド終了時には決して解放しません。

> **誤用は終了させ、呼び出し元に例外を投げません。** 上の `unlock` の各ケースは `std::system_error` を投げますが、それらの呼び出しは `std::unique_lock` / `std::shared_lock` のデストラクタからも来ます。デストラクタは `noexcept` なので、対応の取れない組は `std::terminate` を呼びます。これはフェイルファストで、捕捉できません。ヘッダーのコメントを参照してください。

このコンポーネントの正しさは、下にある `std::shared_mutex` の正しさを超えません。[tests/backend_contract.cpp](tests/backend_contract.cpp) が現在のツールチェーンでその契約を検証します。新しいコンパイラや標準ライブラリを信用する前に実行してください。

移行ガイドとテストの組み合わせは [docs/migration.md](docs/migration.md) を参照してください。

## 書き手の飢餓の観測（R2）

書き手を優先しないバックエンドは、読みの負荷が続くと書き手を飢えさせます。glibc の `pthread_rwlock` や Win32 の SRWLOCK がその例です。任意で使える計測付きガードが、中核コンポーネントに触れずに書き込みの待ち時間と保持時間を測ります：

```cpp
#include <recursive_shared_mutex/instrumented_write_lock.hpp>

// 起動時に一度だけ設置する（すべてのガードより長生きさせること）：
rsm::metrics::AtomicStatsRecorder stats;
rsm::metrics::set_recorder(&stats);

// ホットパス：
{
    rsm::metrics::instrumented_write_lock guard(mtx); // 待ちと保持を記録する
    // ... 排他の処理 ...
}
// stats.snapshot() -> rsm_write_wait_seconds / rsm_write_hold_seconds に流す
```

記録器を設置しなければガードのコストはゼロです。指標とアラートの契約は `docs/roadmap.md` の R2 を参照してください。

## 制約

- TLS のスロット表はモジュールごとです。DLL や EXE の境界が関わる場合は、1 つのミューテックスの `lock_shared` / `unlock_shared` の並びを単一モジュールの TLS 領域に収めるか、TLS の状態を 1 つの公開モジュールに置いてください。これを破ると単にスロットが取り残されるのではなく未定義動作になります。[examples/cross_module_hazard](examples/cross_module_hazard) が実際の DLL/.so で再現します。
- 現在のスレッドが共有ロックを保持したままミューテックスを破棄するのは未定義動作です。デバッグビルドはアサートします。`NDEBUG` か `RSM_NO_DEBUG_ASSERT` の場合はデストラクタが被害を抑えるためにそのスレッドのスロットを黙って捨てますが、破棄が安全になるわけではありません。
- 1 つのスレッドが同時に保持できる共有ロックは `kSharedSlotCount`（256）まで、スレッドあたり約 4 KB の TLS です。次の `lock_shared` は `resource_unavailable_try_again` を投げ、`try_lock_shared` は `false` を返します。スロットは解放時に再利用されるので、多数のミューテックスを順に触るのは問題ありません。
- 共有から排他への昇格はありません。`lock()` は `resource_deadlock_would_occur` を投げます。読み手を `shared_lock` へ移す前に、読み→書きの呼び出し連鎖を監査してください。
- 下にある `std::shared_mutex` は書き手を優先しないため、読みが続くと書き手が飢えることがあります（ベンチマークの S1/S5 を参照）。
- `std::condition_variable_any` は再帰の深さが 1 を超えると安全ではありません。`wait()` は 1 段しか解放しないので、ミューテックスは保持されたままで待ち手は永久に起きません。
- バックエンドは相互排他を守らなければなりません。MinGW-w64 GCC 13.2（libstdc++/winpthreads）は守りません。`try_lock()` が 2 つの所有者を認めてしまい、`unlock()` から誤った `operation_not_permitted` が出ます。MinGW でネイティブのバックエンドを既定にし、`RSM_BACKEND_STD` を任意にしているのはこのためです。新しいツールチェーンでは必ず [tests/backend_contract.cpp](tests/backend_contract.cpp) を実行してください。詳細は [docs/platform-notes.md](docs/platform-notes.md) にあります。
- **文字どおりの `Cpp17Lockable` ではありません。** `unlock()` と `unlock_shared()` は誤用時に `std::system_error` を投げますが、名前付き要件は *Throws: Nothing*（`[thread.mutex.requirements.mutex]`）としています。汎用アルゴリズムは自分が取得したものしか解放しないので、`std::lock`、`std::scoped_lock`、2 つの RAII ラッパーは問題なく動きます。デストラクタから到達した対応の取れない組は、上述のとおり `std::terminate` を呼びます。
- **MinGW では `<windows.h>` を引き込みます。** 既定のネイティブのバックエンドがこれをインクルードし、コンパイル時間が実際に増えます。この数値の出たマシンでは翻訳単位 1 つあたり 655 ms 対 207 ms（3.2×）。`RSM_BACKEND_STD` はインクルードを避けられますが、そのツールチェーンでは不健全なので、うまく外す方法がありません。

## リポジトリ構成

```text
include/recursive_shared_mutex/   # 公開ヘッダー
tests/                            # Catch2 のテスト＋フレームワーク非依存の回帰テスト
examples/                         # 使い方のデモ（モジュール境界の危険の再現を含む）
benchmarks/                       # ドライバ、CI 自動化、結果の記録
docs/                             # 設計、移行、プラットフォームのメモ、ロードマップ、検証
verification/                     # ソークの履歴、記録したバッチ 1 行ずつ
scripts/repo_rules.py             # AGENTS.md が CI で守ると約束している規則（RepoRules の同梱版）
cmake/                            # パッケージ設定
.github/                          # ワークフロー、issue/PR テンプレート、CONTRIBUTING / SECURITY / CODE_OF_CONDUCT
AGENTS.md                         # リポジトリの規則。すべてのエージェントにとって 1 ファイル
```

## コントリビュート

[CONTRIBUTING.md](.github/CONTRIBUTING.md) を参照してください。大きな設計変更の前には issue を立ててください。

リポジトリの規則は [AGENTS.md](AGENTS.md) にあり、Claude Code、Codex、Cursor、Copilot などすべてに対して 1 ファイルです。`CLAUDE.md`、`.cursor/rules/repo.mdc`、`.github/copilot-instructions.md` はそこを指しています。機械的な規則（UTF-8 BOM と CRLF、140 桁、`#pragma comment(lib)` の禁止、ヘッダーの衛生、バージョンの同期、CHANGELOG の節、訳文がリンクされたままであること）は、push のたびに RepoRules（`scripts/repo_rules.py`）が CI で検査するので、レビューを待たずにビルドが落ちます。機械的なものは `python3 scripts/repo_rules.py fix --config repo-rules.json` で直せます。コミット前に検査したければ `git config core.hooksPath .githooks` を実行してください。

## セキュリティ

[SECURITY.md](.github/SECURITY.md) を参照してください。

## ライセンス

[MIT](LICENSE) © 2026 yanxijian
