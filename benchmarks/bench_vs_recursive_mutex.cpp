// Benchmark: RecursiveSharedMutex vs std::recursive_mutex (C++17)
// One configuration per process (recommended on MinGW after shared_mutex stress).
//
// Output line:
//   scenario|impl|R/W|readOps|writeOps|seconds
//
// Example:
//   bench_rsm --impl rsm --mode rw --scenario "S1" --readers 15 --writers 1 --seconds 3

#include <recursive_shared_mutex/recursive_shared_mutex.hpp>

using RecursiveSharedMutex = rsm::RecursiveSharedMutex;
using CacheAlignedRecursiveSharedMutex = rsm::CacheAlignedRecursiveSharedMutex;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace
{

	using Clock = std::chrono::steady_clock;

	// L0 telemetry: how much of the measured time was spent failing to acquire
	// and yielding, and how much CPU the process burned in user vs kernel mode.
	// A contention storm shows up as a high failed-try count and kernel share.
	struct CpuTimes
	{
		double userMs = 0.0;
		double kernelMs = 0.0;
	};

	CpuTimes process_cpu_times()
	{
#if defined(_WIN32)
		FILETIME creation{}, exitTime{}, kernel{}, user{};
		if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
		{
			return {};
		}
		auto to_ms = [](const FILETIME& ft) {
			ULARGE_INTEGER v;
			v.LowPart = ft.dwLowDateTime;
			v.HighPart = ft.dwHighDateTime;
			return static_cast<double>(v.QuadPart) / 10000.0;
		};
		return {to_ms(user), to_ms(kernel)};
#else
		rusage usage{};
		getrusage(RUSAGE_SELF, &usage);
		auto to_ms = [](const timeval& tv) { return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; };
		return {to_ms(usage.ru_utime), to_ms(usage.ru_stime)};
#endif
	}

	std::atomic<std::uint64_t> g_yields{0};
	std::atomic<std::uint64_t> g_failed{0};
	std::int64_t g_userMs = 0;
	std::int64_t g_kernelMs = 0;
	constexpr std::size_t kDataSize = 1u << 20;

	struct Args
	{
		std::string impl = "recursive_mutex"; // recursive_mutex | rsm | rsm_exclusive
		std::string mode = "rw";              // rw | mixed | sharded
		std::string scenario = "bench";
		int readers = 8;
		int writers = 8;
		int workers = 16;
		double writeFraction = 0.2;
		double seconds = 3.0;
		bool exclusiveReads = false;
	};

	std::string getOpt(int argc, char** argv, const char* key, const std::string& def)
	{
		for (int i = 1; i + 1 < argc; ++i)
		{
			if (std::strcmp(argv[i], key) == 0)
			{
				return argv[i + 1];
			}
		}
		return def;
	}

	bool hasFlag(int argc, char** argv, const char* key)
	{
		for (int i = 1; i < argc; ++i)
		{
			if (std::strcmp(argv[i], key) == 0)
			{
				return true;
			}
		}
		return false;
	}

	Args parseArgs(int argc, char** argv)
	{
		Args a;
		a.impl = getOpt(argc, argv, "--impl", a.impl);
		a.mode = getOpt(argc, argv, "--mode", a.mode);
		a.scenario = getOpt(argc, argv, "--scenario", a.scenario);
		a.readers = std::atoi(getOpt(argc, argv, "--readers", "8").c_str());
		a.writers = std::atoi(getOpt(argc, argv, "--writers", "8").c_str());
		a.workers = std::atoi(getOpt(argc, argv, "--workers", "16").c_str());
		a.writeFraction = std::atof(getOpt(argc, argv, "--write-fraction", "0.2").c_str());
		a.seconds = std::atof(getOpt(argc, argv, "--seconds", "3").c_str());
		a.exclusiveReads = hasFlag(argc, argv, "--exclusive-reads");
		return a;
	}

	std::vector<std::uint32_t> makeDataset()
	{
		std::vector<std::uint32_t> data(kDataSize);
		std::mt19937 rng(0xC0FFEEu);
		for (auto& v : data)
		{
			v = rng();
		}
		return data;
	}

	template <class LockFn, class UnlockFn>
	std::uint64_t nestedReadHeld(const std::vector<std::uint32_t>& data, std::size_t index, LockFn lockFn, UnlockFn unlockFn)
	{
		const std::size_t i0 = index % data.size();
		std::uint64_t acc = data[i0];
		lockFn();
		const std::size_t i1 = (i0 * 1315423911u + 1u) % data.size();
		acc = (acc * 1315423911u) ^ data[i1];
		{
			lockFn();
			const std::size_t i2 = (i1 * 2654435761u + 17u) % data.size();
			for (std::size_t k = 0; k < 24; ++k)
			{
				acc = (acc * 11400714819323198485ull) ^ data[(i2 + k) % data.size()];
			}
			unlockFn();
		}
		unlockFn();
		return acc;
	}

	template <class LockFn, class UnlockFn>
	void nestedWriteHeld(std::vector<std::uint32_t>& data, std::size_t index, std::uint32_t value, LockFn lockFn, UnlockFn unlockFn)
	{
		const std::size_t i0 = index % data.size();
		data[i0] = value;
		lockFn();
		const std::size_t i1 = (i0 * 2654435761u + 3u) % data.size();
		data[i1] = value ^ 0xA5A5A5A5u;
		{
			lockFn();
			const std::size_t base = (i1 + 64u) % data.size();
			for (std::size_t k = 0; k < 12; ++k)
			{
				data[(base + k) % data.size()] ^= static_cast<std::uint32_t>(value + static_cast<std::uint32_t>(k));
			}
			unlockFn();
		}
		unlockFn();
	}

	template <class Mutex, bool SharedReads>
	void runRw(Mutex& mtx, std::vector<std::uint32_t>& data, int readers, int writers, double seconds, std::uint64_t& readOps,
	           std::uint64_t& writeOps)
	{
		std::atomic<std::uint64_t> reads{0};
		std::atomic<std::uint64_t> writes{0};
		std::atomic<bool> go{false};
		std::atomic<bool> stop{false};
		std::vector<std::thread> threads;
		g_yields.store(0, std::memory_order_relaxed);
		g_failed.store(0, std::memory_order_relaxed);

		for (int i = 0; i < readers; ++i)
		{
			threads.emplace_back([&, i]() {
				while (!go.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
				std::mt19937_64 rng(0xA11ull + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ull);
				std::uint64_t local = 0;
				std::uint64_t chk = 0;
				while (!stop.load(std::memory_order_relaxed))
				{
					const std::size_t idx = static_cast<std::size_t>(rng());
					if constexpr (SharedReads)
					{
						if (mtx.try_lock_shared())
						{
							chk ^= nestedReadHeld(data, idx, [&] { mtx.lock_shared(); }, [&] { mtx.unlock_shared(); });
							mtx.unlock_shared();
							++local;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
					else
					{
						if (mtx.try_lock())
						{
							chk ^= nestedReadHeld(data, idx, [&] { mtx.lock(); }, [&] { mtx.unlock(); });
							mtx.unlock();
							++local;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
				}
				reads.fetch_add(local, std::memory_order_relaxed);
				(void)chk;
			});
		}

		for (int i = 0; i < writers; ++i)
		{
			threads.emplace_back([&, i]() {
				while (!go.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
				std::mt19937_64 rng(0xB0Bull + static_cast<std::uint64_t>(i) * 0xBF58476D1CE4E5B9ull);
				std::uint64_t local = 0;
				while (!stop.load(std::memory_order_relaxed))
				{
					const std::size_t idx = static_cast<std::size_t>(rng());
					const std::uint32_t value = static_cast<std::uint32_t>(rng());
					if (mtx.try_lock())
					{
						nestedWriteHeld(data, idx, value, [&] { mtx.lock(); }, [&] { mtx.unlock(); });
						mtx.unlock();
						++local;
					}
					else
					{
						std::this_thread::yield();
					}
				}
				writes.fetch_add(local, std::memory_order_relaxed);
			});
		}

		const CpuTimes cpu0 = process_cpu_times();
		go.store(true, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
		stop.store(true, std::memory_order_relaxed);
		for (auto& t : threads)
		{
			t.join();
		}
		const CpuTimes cpu1 = process_cpu_times();
		g_userMs = static_cast<std::int64_t>(cpu1.userMs - cpu0.userMs + 0.5);
		g_kernelMs = static_cast<std::int64_t>(cpu1.kernelMs - cpu0.kernelMs + 0.5);
		readOps = reads.load();
		writeOps = writes.load();
	}

	template <class Mutex, bool SharedReads>
	void runMixed(Mutex& mtx, std::vector<std::uint32_t>& data, int workers, double writeFraction, double seconds, std::uint64_t& readOps,
	              std::uint64_t& writeOps)
	{
		std::atomic<std::uint64_t> reads{0};
		std::atomic<std::uint64_t> writes{0};
		std::atomic<bool> go{false};
		std::atomic<bool> stop{false};
		std::vector<std::thread> threads;
		g_yields.store(0, std::memory_order_relaxed);
		g_failed.store(0, std::memory_order_relaxed);

		for (int i = 0; i < workers; ++i)
		{
			threads.emplace_back([&, i]() {
				while (!go.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
				std::mt19937_64 rng(0xC0Dull + static_cast<std::uint64_t>(i) * 0x94D049BB133111EBull);
				std::uniform_real_distribution<double> uni(0.0, 1.0);
				std::uint64_t localR = 0;
				std::uint64_t localW = 0;
				while (!stop.load(std::memory_order_relaxed))
				{
					const std::size_t idx = static_cast<std::size_t>(rng());
					if (uni(rng) < writeFraction)
					{
						if (mtx.try_lock())
						{
							nestedWriteHeld(data, idx, static_cast<std::uint32_t>(rng()), [&] { mtx.lock(); }, [&] { mtx.unlock(); });
							mtx.unlock();
							++localW;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
					else if constexpr (SharedReads)
					{
						if (mtx.try_lock_shared())
						{
							(void)nestedReadHeld(data, idx, [&] { mtx.lock_shared(); }, [&] { mtx.unlock_shared(); });
							mtx.unlock_shared();
							++localR;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
					else
					{
						if (mtx.try_lock())
						{
							(void)nestedReadHeld(data, idx, [&] { mtx.lock(); }, [&] { mtx.unlock(); });
							mtx.unlock();
							++localR;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
				}
				reads.fetch_add(localR, std::memory_order_relaxed);
				writes.fetch_add(localW, std::memory_order_relaxed);
			});
		}

		const CpuTimes cpu0 = process_cpu_times();
		go.store(true, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
		stop.store(true, std::memory_order_relaxed);
		for (auto& t : threads)
		{
			t.join();
		}
		const CpuTimes cpu1 = process_cpu_times();
		g_userMs = static_cast<std::int64_t>(cpu1.userMs - cpu0.userMs + 0.5);
		g_kernelMs = static_cast<std::int64_t>(cpu1.kernelMs - cpu0.kernelMs + 0.5);
		readOps = reads.load();
		writeOps = writes.load();
	}

	template <class Mutex, bool SharedReads>
	void runSharded(std::vector<Mutex>& mtxs, std::vector<std::uint32_t>& data, int workers, double writeFraction, double seconds,
	                std::uint64_t& readOps, std::uint64_t& writeOps)
	{
		std::atomic<std::uint64_t> reads{0};
		std::atomic<std::uint64_t> writes{0};
		std::atomic<bool> go{false};
		std::atomic<bool> stop{false};
		std::vector<std::thread> threads;
		g_yields.store(0, std::memory_order_relaxed);
		g_failed.store(0, std::memory_order_relaxed);
		const int shards = static_cast<int>(mtxs.size());

		for (int i = 0; i < workers; ++i)
		{
			threads.emplace_back([&, i]() {
				while (!go.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
				std::mt19937_64 rng(0xD00Dull + static_cast<std::uint64_t>(i));
				std::uniform_int_distribution<int> shardDist(0, shards - 1);
				std::uniform_real_distribution<double> uni(0.0, 1.0);
				std::uint64_t localR = 0;
				std::uint64_t localW = 0;
				while (!stop.load(std::memory_order_relaxed))
				{
					Mutex& mtx = mtxs[static_cast<std::size_t>(shardDist(rng))];
					const std::size_t idx = static_cast<std::size_t>(rng());
					if (uni(rng) < writeFraction)
					{
						if (mtx.try_lock())
						{
							nestedWriteHeld(data, idx, static_cast<std::uint32_t>(rng()), [&] { mtx.lock(); }, [&] { mtx.unlock(); });
							mtx.unlock();
							++localW;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
					else if constexpr (SharedReads)
					{
						if (mtx.try_lock_shared())
						{
							(void)nestedReadHeld(data, idx, [&] { mtx.lock_shared(); }, [&] { mtx.unlock_shared(); });
							mtx.unlock_shared();
							++localR;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
					else
					{
						if (mtx.try_lock())
						{
							(void)nestedReadHeld(data, idx, [&] { mtx.lock(); }, [&] { mtx.unlock(); });
							mtx.unlock();
							++localR;
						}
						else
						{
							g_failed.fetch_add(1, std::memory_order_relaxed);
							g_yields.fetch_add(1, std::memory_order_relaxed);
							std::this_thread::yield();
						}
					}
				}
				reads.fetch_add(localR, std::memory_order_relaxed);
				writes.fetch_add(localW, std::memory_order_relaxed);
			});
		}

		const CpuTimes cpu0 = process_cpu_times();
		go.store(true, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
		stop.store(true, std::memory_order_relaxed);
		for (auto& t : threads)
		{
			t.join();
		}
		const CpuTimes cpu1 = process_cpu_times();
		g_userMs = static_cast<std::int64_t>(cpu1.userMs - cpu0.userMs + 0.5);
		g_kernelMs = static_cast<std::int64_t>(cpu1.kernelMs - cpu0.kernelMs + 0.5);
		readOps = reads.load();
		writeOps = writes.load();
	}

	template <class Mutex, bool SharedReads> void dispatch(const Args& a, const std::string& implName)
	{
		std::vector<std::uint32_t> data = makeDataset();
		std::uint64_t readOps = 0;
		std::uint64_t writeOps = 0;
		std::string rwLabel;

		if (a.mode == "rw")
		{
			Mutex mtx;
			runRw<Mutex, SharedReads>(mtx, data, a.readers, a.writers, a.seconds, readOps, writeOps);
			rwLabel = std::to_string(a.readers) + "/" + std::to_string(a.writers);
		}
		else if (a.mode == "mixed")
		{
			Mutex mtx;
			runMixed<Mutex, SharedReads>(mtx, data, a.workers, a.writeFraction, a.seconds, readOps, writeOps);
			rwLabel = "N=" + std::to_string(a.workers);
		}
		else
		{
			std::vector<Mutex> mtxs(8);
			runSharded<Mutex, SharedReads>(mtxs, data, a.workers, a.writeFraction, a.seconds, readOps, writeOps);
			rwLabel = "N=" + std::to_string(a.workers);
		}

		std::cout << a.scenario << "|" << implName << "|" << rwLabel << "|" << readOps << "|" << writeOps << "|" << a.seconds << "|"
		          << g_yields.load(std::memory_order_relaxed) << "|" << g_failed.load(std::memory_order_relaxed) << "|" << g_userMs << "|"
		          << g_kernelMs << "\n";
	}

} // namespace

int main(int argc, char** argv)
{
	// --backend prints which backend the numbers below would come from and
	// exits. benchmarks/run_bench.ps1 records it next to the results, because
	// "MinGW numbers" means something different before and after the native
	// backend became the default there.
	if (hasFlag(argc, argv, "--backend"))
	{
#if defined(RSM_BACKEND_SRWLOCK)
		const char* name = "rsm::detail::SrwLockBackend";
#else
		const char* name = "std::shared_mutex";
#endif
		std::printf("backend|%s|%zu\n", name, sizeof(rsm::RecursiveSharedMutex::backend_type));
		return 0;
	}

	const Args a = parseArgs(argc, argv);
	if (a.impl == "recursive_mutex")
	{
		dispatch<std::recursive_mutex, false>(a, "recursive_mutex");
	}
	else if (a.impl == "rsm_exclusive" || (a.impl == "rsm" && a.exclusiveReads))
	{
		dispatch<RecursiveSharedMutex, false>(a, "RSM exclusive-only");
	}
	else if (a.impl == "rsm_padded")
	{
		// Same component, cache-line-aligned instance. Meaningful for --mode
		// sharded, where the 24-byte default packs ~2.6 mutexes per cache line.
		dispatch<CacheAlignedRecursiveSharedMutex, true>(a, "RSM cache-aligned");
	}
	else
	{
		dispatch<RecursiveSharedMutex, true>(a, "RecursiveSharedMutex");
	}
	return 0;
}
