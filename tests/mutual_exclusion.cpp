// Library-level mutual-exclusion test for rsm::RecursiveSharedMutex.
//
// The repo's existing suite covers reentrancy bookkeeping and every error path,
// but no test asserts the property the type exists to provide: two writers are
// never inside the critical section at the same time, and no reader is inside
// while a writer is. (`a == b` paired increments do NOT catch a broken
// exclusive lock - both writers just increment both sides.)
//
// Self-contained: standard library + the library header only.
// Build:  c++ -O2 -std=c++17 -pthread -I<repo>/include mutual_exclusion.cpp
//         cl /O2 /std:c++17 /EHsc /I<repo>\include mutual_exclusion.cpp
// Run:    ./mutual_exclusion [--threads N] [--iters N] [--seconds S]
// Exit:   0 = no violation, 1 = violation, 2 = bad usage.
//
// Failure modes and what they mean:
//  - "exclusive overlaps"     -> the lock itself is broken (or its backend is).
//  - "reader with writer"     -> shared/exclusive exclusion is broken.
//  - "spurious errors"        -> recursion accounting was corrupted; on MinGW
//                                this is the known winpthreads try_lock defect,
//                                run backend_contract.cpp to confirm.
//  - "lock left held at exit" -> accounting leaked (stranded slot / bad unlock).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

	struct Options
	{
		int threads = 8;
		int iters = 100000;
		int seconds = 0; // >0: wall-clock mode, iters ignored
	};

	struct Counters
	{
		std::atomic<int> exclusiveInside{0};
		std::atomic<long long> exclusiveWins{0};
		std::atomic<long long> sharedEntries{0};
		std::atomic<long long> exclusiveOverlaps{0};
		std::atomic<long long> readerWithWriter{0};
		std::atomic<long long> spuriousErrors{0};
	};

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
			else if (std::strcmp(argv[i], "--seconds") == 0)
			{
				o.seconds = std::atoi(argv[i + 1]);
			}
		}
		if (o.threads < 2 || (o.iters < 1 && o.seconds < 1))
		{
			std::printf("usage: mutual_exclusion [--threads N>=2] [--iters N>=1] [--seconds S>=1]\n");
			std::exit(2);
		}
		return o;
	}

	// Exclusive scope. `depth` is the thread's own recursion depth, so only the
	// outermost acquisition participates in the global in/out bookkeeping.
	class ExclusiveScope
	{
	public:
		ExclusiveScope(rsm::RecursiveSharedMutex& m, Counters& c, int& depth)
		    : m_(m),
		      c_(c),
		      depth_(depth),
		      outer_(false)
		{
			m_.lock();
			outer_ = (depth_++ == 0);
			if (outer_ && c_.exclusiveInside.fetch_add(1, std::memory_order_seq_cst) != 0)
			{
				++c_.exclusiveOverlaps;
			}
		}

		~ExclusiveScope()
		{
			--depth_;
			if (outer_)
			{
				c_.exclusiveInside.fetch_sub(1, std::memory_order_seq_cst);
				++c_.exclusiveWins;
			}
			m_.unlock();
		}

		ExclusiveScope(const ExclusiveScope&) = delete;
		ExclusiveScope& operator=(const ExclusiveScope&) = delete;

	private:
		rsm::RecursiveSharedMutex& m_;
		Counters& c_;
		int& depth_;
		bool outer_;
	};

	class SharedScope
	{
	public:
		// `exclusiveDepth` is the calling thread's own write-recursion depth: shared
		// locks taken *while this thread holds the write lock* are legal (the
		// component counts them in under_exclusive_) and must not be flagged.
		SharedScope(rsm::RecursiveSharedMutex& m, Counters& c, int exclusiveDepth)
		    : m_(m),
		      c_(c)
		{
			m_.lock_shared();
			if (exclusiveDepth == 0 && c_.exclusiveInside.load(std::memory_order_seq_cst) != 0)
			{
				++c_.readerWithWriter;
			}
		}

		~SharedScope()
		{
			m_.unlock_shared();
		}

		SharedScope(const SharedScope&) = delete;
		SharedScope& operator=(const SharedScope&) = delete;

	private:
		rsm::RecursiveSharedMutex& m_;
		Counters& c_;
	};

	void run(rsm::RecursiveSharedMutex& mtx, Counters& c, const Options& opt, int id)
	{
		std::mt19937 rng(0x5EEDu + static_cast<unsigned>(id) * 2654435761u);
		const auto deadline = opt.seconds > 0 ? std::chrono::steady_clock::now() + std::chrono::seconds(opt.seconds)
		                                      : std::chrono::steady_clock::time_point::max();
		int depth = 0;

		for (int i = 0;; ++i)
		{
			if (opt.seconds > 0)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					break;
				}
			}
			else if (i >= opt.iters)
			{
				break;
			}

			try
			{
				switch (rng() % 8)
				{
				case 0: // exclusive, nested twice
				case 1: {
					ExclusiveScope outer(mtx, c, depth);
					{
						ExclusiveScope inner(mtx, c, depth);
					}
					++c.sharedEntries; // cheap work, keeps the compiler honest
					break;
				}
				case 2: // exclusive -> nested shared -> exclusive continues
				{
					ExclusiveScope outer(mtx, c, depth);
					{
						SharedScope shared(mtx, c, depth);
						++c.sharedEntries;
					}
					break;
				}
				case 3: // shared, nested twice
				case 4: {
					SharedScope a(mtx, c, depth);
					{
						SharedScope b(mtx, c, depth);
					}
					++c.sharedEntries;
					break;
				}
				case 5: // try variants
				{
					if (mtx.try_lock())
					{
						const bool outer = (depth++ == 0);
						if (outer && c.exclusiveInside.fetch_add(1, std::memory_order_seq_cst) != 0)
						{
							++c.exclusiveOverlaps;
						}
						++c.sharedEntries;
						--depth;
						if (outer)
						{
							c.exclusiveInside.fetch_sub(1, std::memory_order_seq_cst);
							++c.exclusiveWins;
						}
						mtx.unlock();
					}
					else if (mtx.try_lock_shared())
					{
						if (c.exclusiveInside.load(std::memory_order_seq_cst) != 0)
						{
							++c.readerWithWriter;
						}
						mtx.unlock_shared();
						++c.sharedEntries;
					}
					break;
				}
				default: // single shared
				{
					SharedScope shared(mtx, c, depth);
					++c.sharedEntries;
					break;
				}
				}
			}
			catch (const std::system_error&)
			{
				// The table-exhaustion path is a legitimate throw; anything else here
				// means the recursion accounting got corrupted underneath us.
				++c.spuriousErrors;
			}
			catch (...)
			{
				++c.spuriousErrors;
			}
		}
	}

} // namespace

int main(int argc, char** argv)
{
	const Options opt = parse(argc, argv);
	rsm::RecursiveSharedMutex mtx;
	Counters c;

	std::printf("rsm::RecursiveSharedMutex mutual-exclusion test\n  toolchain: %s\n", toolchain().c_str());
	std::printf("  threads=%d %s\n", opt.threads,
	            opt.seconds > 0 ? ("seconds=" + std::to_string(opt.seconds)).c_str()
	                            : ("iters=" + std::to_string(opt.iters)).c_str());

	std::vector<std::thread> ts;
	ts.reserve(static_cast<std::size_t>(opt.threads));
	for (int t = 0; t < opt.threads; ++t)
	{
		ts.emplace_back([&, t] { run(mtx, c, opt, t); });
	}
	for (auto& t : ts)
	{
		t.join();
	}

	// Nothing may be left held, and the mutex must be fully usable afterwards.
	bool stranded = false;
	try
	{
		if (!mtx.try_lock())
		{
			stranded = true;
		}
		else
		{
			mtx.unlock();
		}
		if (mtx.try_lock_shared())
		{
			mtx.unlock_shared();
		}
		else
		{
			stranded = true;
		}
	}
	catch (const std::system_error&)
	{
		stranded = true;
	}

	std::printf("  exclusive wins      : %lld\n", c.exclusiveWins.load());
	std::printf("  shared entries      : %lld\n", c.sharedEntries.load());
	std::printf("  exclusive overlaps  : %lld\n", c.exclusiveOverlaps.load());
	std::printf("  reader with writer  : %lld\n", c.readerWithWriter.load());
	std::printf("  spurious errors     : %lld\n", c.spuriousErrors.load());
	std::printf("  lock left held      : %s\n", stranded ? "YES" : "no");

	const long long failures =
	    c.exclusiveOverlaps.load() + c.readerWithWriter.load() + c.spuriousErrors.load() + (stranded ? 1 : 0);

	if (c.exclusiveWins.load() == 0 && c.sharedEntries.load() > 0)
	{
		std::printf("  WARNING: writers never won (starvation or a very short run).\n");
	}

	if (failures == 0)
	{
		std::printf("VERDICT: mutual exclusion holds.\n");
		return 0;
	}
	std::printf("VERDICT: FAILED (%lld problem events).\n", failures);
	if (c.spuriousErrors.load() > 0)
	{
		std::printf("  Spurious system_error usually means the backend granted two owners:\n");
		std::printf("  run backend_contract.cpp on this toolchain to confirm.\n");
	}
	return 1;
}
