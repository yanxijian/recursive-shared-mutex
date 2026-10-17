// Backend contract check for the mutex the library is built on.
//
// With RSM_BACKEND_SRWLOCK this checks the SRWLOCK backend; otherwise it checks
// std::shared_mutex. Build both on Windows/MinGW: the flag is what makes the
// contract hold there.
//
// Purpose: `rsm::RecursiveSharedMutex` (and any recursive shared mutex) is only
// as correct as its backend. On MinGW-w64 GCC 13.2 (libstdc++ over winpthreads)
// `std::shared_mutex::try_lock()` grants two exclusive owners under a
// try_lock/yield storm (~1/1200 successful acquisitions), and a reader can be
// inside while a writer is inside. That defect silently breaks every wrapper
// built on top of it, so it must be checked per toolchain.
//
// Self-contained: standard library only, no test framework, no CMake needed.
// Build:  c++ -O2 -std=c++17 -pthread backend_contract.cpp -o backend_contract
//         cl /O2 /std:c++17 /EHsc backend_contract.cpp
// Run:    ./backend_contract [--threads N] [--iters N] [--rounds N]
// Exit:   0 = contract holds, 1 = violation, 2 = bad usage.
//
// A long critical section (~200 ns of dependent work) is used so that a
// violation cannot hide between two adjacent atomic operations.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

	// The backend the library is actually built on. With RSM_BACKEND_SRWLOCK
	// this is rsm::detail::SrwLockBackend, otherwise std::shared_mutex.
	using Backend = rsm::RecursiveSharedMutex::backend_type;

	const char* backendName()
	{
#if defined(RSM_BACKEND_SRWLOCK)
		return "rsm::detail::SrwLockBackend (RSM_BACKEND_SRWLOCK)";
#else
		return "std::shared_mutex";
#endif
	}

	struct Options
	{
		int threads = 8;
		int iters = 3000;
		int rounds = 20;
	};

	void burn()
	{
		volatile long long x = 0;
		for (int k = 0; k < 200; ++k)
		{
			x += k;
		}
		(void)x;
	}

	std::string toolchain()
	{
#if defined(_MSC_VER) && defined(__clang__)
		return "clang-cl/MSVC_STL _MSC_VER=" + std::to_string(_MSC_VER);
#elif defined(_MSC_VER)
		return "MSVC/MSVC_STL _MSC_VER=" + std::to_string(_MSC_VER);
#elif defined(__clang__) && defined(__GLIBCXX__)
		return std::string("clang/libstdc++ ") + __VERSION__;
#elif defined(__clang__)
		return std::string("clang/libc++ ") + __VERSION__;
#elif defined(__GNUC__)
		return std::string("gcc/libstdc++ ") + __VERSION__;
#else
		return "unknown toolchain";
#endif
	}

	// (1) Two threads must never hold the same mutex exclusively at the same time.
	long long exclusiveVsExclusive(const Options& opt)
	{
		long long violations = 0;
		long long ops = 0;
		for (int r = 0; r < opt.rounds; ++r)
		{
			Backend m;
			std::atomic<int> inside{0};
			std::atomic<long long> localOps{0};
			std::atomic<bool> go{false};
			std::vector<std::thread> ts;
			ts.reserve(static_cast<std::size_t>(opt.threads));
			for (int t = 0; t < opt.threads; ++t)
			{
				ts.emplace_back([&] {
					while (!go.load(std::memory_order_acquire))
					{
					}
					for (int i = 0; i < opt.iters; ++i)
					{
						if (!m.try_lock())
						{
							std::this_thread::yield();
							continue;
						}
						if (inside.fetch_add(1, std::memory_order_seq_cst) != 0)
						{
							++violations;
						}
						burn();
						inside.fetch_sub(1, std::memory_order_seq_cst);
						m.unlock();
						localOps.fetch_add(1, std::memory_order_relaxed);
					}
				});
			}
			go.store(true, std::memory_order_release);
			for (auto& t : ts)
			{
				t.join();
			}
			ops += localOps.load();
		}
		std::printf("  [1] exclusive-vs-exclusive: ops=%lld violations=%lld\n", ops, violations);
		return violations;
	}

	// (2) A reader must never be inside while a writer is inside.
	long long writerVsReader(const Options& opt)
	{
		long long violations = 0;
		long long ops = 0;
		for (int r = 0; r < opt.rounds; ++r)
		{
			Backend m;
			std::atomic<int> writers{0};
			std::atomic<long long> localOps{0};
			std::atomic<bool> go{false};
			std::vector<std::thread> ts;
			ts.reserve(static_cast<std::size_t>(opt.threads));
			for (int t = 0; t < opt.threads; ++t)
			{
				ts.emplace_back([&, t] {
					while (!go.load(std::memory_order_acquire))
					{
					}
					for (int i = 0; i < opt.iters; ++i)
					{
						const bool asWriter = (t == 0) && (i % 8 == 0);
						if (asWriter)
						{
							if (!m.try_lock())
							{
								std::this_thread::yield();
								continue;
							}
							writers.fetch_add(1, std::memory_order_seq_cst);
							burn();
							writers.fetch_sub(1, std::memory_order_seq_cst);
							m.unlock();
						}
						else
						{
							if (!m.try_lock_shared())
							{
								std::this_thread::yield();
								continue;
							}
							if (writers.load(std::memory_order_seq_cst) != 0)
							{
								++violations;
							}
							burn();
							m.unlock_shared();
						}
						localOps.fetch_add(1, std::memory_order_relaxed);
					}
				});
			}
			go.store(true, std::memory_order_release);
			for (auto& t : ts)
			{
				t.join();
			}
			ops += localOps.load();
		}
		std::printf("  [2] reader-inside-while-writer-inside: ops=%lld violations=%lld\n", ops, violations);
		return violations;
	}

	// (3) Sanity: try_lock must fail while another thread holds the lock.
	bool tryLockRespectsHolder()
	{
		Backend m;
		m.lock();
		bool lied = false;
		std::thread prober([&] {
			for (int i = 0; i < 1000; ++i)
			{
				if (m.try_lock())
				{
					lied = true;
					m.unlock();
				}
			}
		});
		prober.join();
		m.unlock();
		std::printf("  [3] try_lock against a steady holder: %s\n", lied ? "LIED (violation)" : "ok");
		return !lied;
	}

	Options parse(int argc, char** argv)
	{
		Options o;
		for (int i = 1; i + 1 < argc; ++i)
		{
			if (std::strcmp(argv[i], "--threads") == 0)
			{
				o.threads = std::atoi(argv[i + 1]);
			}
			else if (std::strcmp(argv[i], "--iters") == 0)
			{
				o.iters = std::atoi(argv[i + 1]);
			}
			else if (std::strcmp(argv[i], "--rounds") == 0)
			{
				o.rounds = std::atoi(argv[i + 1]);
			}
		}
		if (o.threads < 2 || o.iters < 1 || o.rounds < 1)
		{
			std::printf("usage: backend_contract [--threads N>=2] [--iters N>=1] [--rounds N>=1]\n");
			std::exit(2);
		}
		return o;
	}

} // namespace

int main(int argc, char** argv)
{
	const Options opt = parse(argc, argv);
	std::printf(
	    "shared_mutex backend contract\n  backend: %s (sizeof=%zu)\n  toolchain: %s\n  threads=%d iters=%d rounds=%d\n",
	    backendName(), sizeof(Backend), toolchain().c_str(), opt.threads, opt.iters, opt.rounds);

	const long long v1 = exclusiveVsExclusive(opt);
	const long long v2 = writerVsReader(opt);
	const bool ok3 = tryLockRespectsHolder();
	const long long violations = v1 + v2 + (ok3 ? 0 : 1);

	if (violations == 0)
	{
		std::printf("VERDICT: backend honours mutual exclusion.\n");
		return 0;
	}
	std::printf("VERDICT: BACKEND VIOLATES MUTUAL EXCLUSION (%lld violations).\n", violations);
	std::printf("  Any recursive-shared-mutex wrapper on this toolchain is unsound.\n");
	std::printf("  Known case: MinGW-w64 + libstdc++/winpthreads. Use a native SRWLOCK/pthread\n");
	std::printf("  backend, or do not ship try_lock-based locking on this toolchain.\n");
	return 1;
}
