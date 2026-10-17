// Model-based concurrency test for rsm::RecursiveSharedMutex.
//
// The fixed suite (test_recursive_shared_mutex.cpp) checks the cases someone
// thought of. This file checks the spec itself against a shadow model over
// random operation sequences, which is what makes the coverage grow with the
// seed count instead of with the number of hand-written cases.
//
// Two phases:
//   1. single-threaded, K mutexes. Fully deterministic given --seed, and the
//      expectations are exact: every call's success, return value or error code
//      is predicted from the model, including per-mutex isolation (the model
//      holds several mutexes at once, so the slot table is exercised).
//   2. multi-threaded, one mutex per thread at a time. Only local invariants are
//      asserted (what the thread's own history decides) plus a global
//      mutual-exclusion counter; the outcome of a contended try_lock is NOT
//      predicted, because the owner may release between the model read and the
//      call. Keeping one mutex per thread also means a blocking lock() cannot
//      deadlock by lock ordering, so any hang is a library bug.
//
// Reproducibility: --seed drives every choice. Phase 1 is bit-for-bit
// deterministic (its op-sequence digest is printed); phase 2 logs each thread's
// last operations in a ring buffer, dumped on the first violation, and the
// watchdog dumps on a stall. A failing run therefore reports the seed and the
// interleaving that produced it.
//
// NOT for RSM_BACKEND_STD on MinGW: that backend is not mutually exclusive, and
// this test assumes the contract (run backend_contract.cpp to confirm it there).
//
// Self-contained: standard library + the library header only.
// Build:  c++ -O2 -std=c++17 -pthread -I<repo>/include model_check.cpp
//         cl /O2 /std:c++17 /EHsc /I<repo>\include model_check.cpp
// Run:    ./model_check [--seed N] [--ops N] [--threads N] [--seconds S] [--mutexes K]
// Exit:   0 = clean, 1 = model mismatch, 2 = bad usage, 3 = stall (watchdog).

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

	using Mutex = rsm::RecursiveSharedMutex;

	constexpr int kMaxThreads = 32;
	constexpr std::uint32_t kRingSize = 1024;
	constexpr std::uint64_t kSeedMix = 0x9E3779B97F4A7C15ULL;

	// What the thread believes about its own relationship to one mutex.
	struct Shadow
	{
		int excl = 0;   // recursive exclusive depth
		int shared = 0; // recursive shared depth (slot depth)
		int under = 0;  // lock_shared() calls nested under the exclusive lock
	};

	enum class Op : std::uint8_t
	{
		Lock,
		TryLock,
		Unlock,
		LockShared,
		TryLockShared,
		UnlockShared,
		Held,
		Count
	};

	constexpr int kOpCount = static_cast<int>(Op::Count);

	const char* opName(Op op)
	{
		switch (op)
		{
		case Op::Lock:
			return "lock";
		case Op::TryLock:
			return "try_lock";
		case Op::Unlock:
			return "unlock";
		case Op::LockShared:
			return "lock_shared";
		case Op::TryLockShared:
			return "try_lock_shared";
		case Op::UnlockShared:
			return "unlock_shared";
		case Op::Held:
			return "is_held_exclusive";
		default:
			return "?";
		}
	}

	// result: 0 = succeeded, 1 = returned false, 2 = threw, 3 = unexpected
	struct Rec
	{
		std::uint32_t iter;
		std::uint8_t op;
		std::uint16_t mutex;
		std::int16_t excl;
		std::int16_t shared;
		std::int16_t under;
		std::uint8_t result;
	};

	struct Ring
	{
		Rec recs[kRingSize];
		std::atomic<std::uint32_t> count{0};
	};

	Ring g_rings[kMaxThreads];
	std::uint64_t g_seed = 0;
	std::atomic<std::uint64_t> g_progress{0};

	void record(int slot, std::uint32_t iter, Op op, int mutex, const Shadow& s, std::uint8_t result)
	{
		if (slot < 0 || slot >= kMaxThreads)
		{
			return;
		}
		const std::uint32_t n = g_rings[slot].count.fetch_add(1, std::memory_order_relaxed);
		Rec& r = g_rings[slot].recs[n % kRingSize];
		r.iter = iter;
		r.op = static_cast<std::uint8_t>(op);
		r.mutex = static_cast<std::uint16_t>(mutex);
		r.excl = static_cast<std::int16_t>(s.excl);
		r.shared = static_cast<std::int16_t>(s.shared);
		r.under = static_cast<std::int16_t>(s.under);
		r.result = result;
	}

	void dumpRings(std::FILE* out)
	{
		std::fprintf(out, "  seed=%llu  (rerun with --seed %llu)\n", static_cast<unsigned long long>(g_seed),
		             static_cast<unsigned long long>(g_seed));
		for (int t = 0; t < kMaxThreads; ++t)
		{
			const std::uint32_t total = g_rings[t].count.load(std::memory_order_relaxed);
			if (total == 0)
			{
				continue;
			}
			const std::uint32_t shown = total < kRingSize ? total : kRingSize;
			std::fprintf(out, "  -- thread %d: last %u of %u operations --\n", t, shown, total);
			for (std::uint32_t i = total - shown; i < total; ++i)
			{
				const Rec& r = g_rings[t].recs[i % kRingSize];
				static const char* kResults[] = {"ok", "false", "threw", "UNEXPECTED"};
				const std::uint8_t result = r.result <= 3 ? r.result : 3;
				std::fprintf(out, "  %8u %-17s m%-2u excl=%d shared=%d under=%d -> %s\n", r.iter, opName(static_cast<Op>(r.op)),
				             static_cast<unsigned>(r.mutex), r.excl, r.shared, r.under, kResults[result]);
			}
		}
		std::fflush(out);
	}

	[[noreturn]] void fail(const char* what, int slot, std::uint32_t iter, const Shadow& s, int mutex)
	{
		std::fprintf(stderr, "\nMODEL CHECK FAILED: %s\n", what);
		std::fprintf(stderr, "  thread=%d iter=%u mutex=%d model={excl=%d shared=%d under=%d}\n", slot, iter, mutex, s.excl, s.shared,
		             s.under);
		dumpRings(stderr);
		std::_Exit(1);
	}

	struct Attempt
	{
		bool threw = false;
		bool value = false;
		std::error_code code;
	};

	template <class F> Attempt attempt(F f)
	{
		Attempt a;
		try
		{
			a.value = f();
		}
		catch (const std::system_error& e)
		{
			a.threw = true;
			a.code = e.code();
		}
		return a;
	}

	void expectCall(const Attempt& a, bool should_throw, std::errc expected, const Shadow& s, int slot, std::uint32_t iter, int mutex,
	                Op op)
	{
		if (a.threw != should_throw)
		{
			fail(opName(op), slot, iter, s, mutex);
		}
		if (should_throw && a.code != expected)
		{
			fail(opName(op), slot, iter, s, mutex);
		}
	}

	// One counter per mutex: exclusion is a per-mutex property, and the workload
	// deliberately spreads threads across K mutexes.
	struct Global
	{
		explicit Global(int mutexCount)
		    : inside(static_cast<std::size_t>(mutexCount))
		{
			for (auto& a : inside)
			{
				a.store(0, std::memory_order_relaxed);
			}
		}

		std::vector<std::atomic<int>> inside;
		std::atomic<long long> exclusiveEntered{0};
		std::atomic<long long> exclusiveLeft{0};
		std::atomic<long long> overlaps{0};
		std::atomic<long long> readerWithWriter{0};
	};

	void enterExclusive(Global& g, int m)
	{
		g.exclusiveEntered.fetch_add(1, std::memory_order_relaxed);
		if (g.inside[static_cast<std::size_t>(m)].fetch_add(1, std::memory_order_seq_cst) != 0)
		{
			g.overlaps.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void leaveExclusive(Global& g, int m)
	{
		g.exclusiveLeft.fetch_add(1, std::memory_order_relaxed);
		g.inside[static_cast<std::size_t>(m)].fetch_sub(1, std::memory_order_seq_cst);
	}

	void enterShared(Global& g, int m)
	{
		if (g.inside[static_cast<std::size_t>(m)].load(std::memory_order_seq_cst) != 0)
		{
			g.readerWithWriter.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// Keep the random walk bounded and biased toward release: with a uniform mix
	// the acquire ops outnumber the release ops, the walk parks at the cap and a
	// thread holds the mutex for the whole run. Half the decisions release when
	// there is something to release, which keeps depth low and acquisition dense.
	constexpr int kMaxDepth = 8;

	Op chooseOp(const Shadow& s, std::mt19937_64& rng)
	{
		const std::uint64_t roll = rng() % 100;
		if (roll < 50)
		{
			// Release preference: `unlock()` throws while a nested shared lock
			// exists, so that must be released first or the thread parks at depth 1.
			if (s.under > 0 || s.shared > 0)
			{
				return Op::UnlockShared;
			}
			if (s.excl > 0)
			{
				return Op::Unlock;
			}
		}
		if (s.excl > kMaxDepth || s.under > kMaxDepth || s.shared > kMaxDepth)
		{
			return s.under > 0 || s.shared > 0 ? Op::UnlockShared : Op::Unlock;
		}
		return static_cast<Op>(rng() % static_cast<std::uint64_t>(kOpCount));
	}

	// ---------------------------------------------------------------------
	// Phase 1: single thread, K mutexes, exact expectations.
	// ---------------------------------------------------------------------
	void applySingle(const std::vector<std::unique_ptr<Mutex>>& mutexes, std::vector<Shadow>& model, int m, Op op, int slot,
	                 std::uint32_t iter, std::uint64_t& digest)
	{
		Mutex& mtx = *mutexes[static_cast<std::size_t>(m)];
		Shadow& s = model[static_cast<std::size_t>(m)];
		digest = (digest ^ static_cast<std::uint64_t>(m)) * 1099511628211ULL;
		digest = (digest ^ static_cast<std::uint64_t>(op)) * 1099511628211ULL;

		std::uint8_t result = 0;
		switch (op)
		{
		case Op::Lock: {
			if (s.excl > 0)
			{
				expectCall(attempt([&] {
					           mtx.lock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				++s.excl;
			}
			else if (s.shared > 0)
			{
				expectCall(attempt([&] {
					           mtx.lock();
					           return true;
				           }),
				           true, std::errc::resource_deadlock_would_occur, s, slot, iter, m, op);
				result = 2;
			}
			else
			{
				expectCall(attempt([&] {
					           mtx.lock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				s.excl = 1;
			}
			break;
		}
		case Op::TryLock: {
			if (s.excl > 0)
			{
				const Attempt a = attempt([&] { return mtx.try_lock(); });
				if (a.threw || !a.value)
				{
					fail(opName(op), slot, iter, s, m);
				}
				++s.excl;
			}
			else if (s.shared > 0)
			{
				const Attempt a = attempt([&] { return mtx.try_lock(); });
				if (a.threw || a.value)
				{
					fail(opName(op), slot, iter, s, m);
				}
				result = 1;
			}
			else
			{
				const Attempt a = attempt([&] { return mtx.try_lock(); });
				if (a.threw || !a.value)
				{
					fail(opName(op), slot, iter, s, m);
				}
				s.excl = 1;
			}
			break;
		}
		case Op::Unlock: {
			if (s.excl <= 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           true, std::errc::operation_not_permitted, s, slot, iter, m, op);
				result = 2;
			}
			else if (s.excl > 1)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				--s.excl;
			}
			else if (s.under > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           true, std::errc::operation_not_permitted, s, slot, iter, m, op);
				result = 2;
			}
			else
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				s.excl = 0;
			}
			break;
		}
		case Op::LockShared: {
			if (s.excl > 0)
			{
				expectCall(attempt([&] {
					           mtx.lock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				++s.under;
			}
			else
			{
				expectCall(attempt([&] {
					           mtx.lock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				++s.shared;
			}
			break;
		}
		case Op::TryLockShared: {
			const Attempt a = attempt([&] { return mtx.try_lock_shared(); });
			if (a.threw || !a.value)
			{
				fail(opName(op), slot, iter, s, m);
			}
			if (s.excl > 0)
			{
				++s.under;
			}
			else
			{
				++s.shared;
			}
			break;
		}
		case Op::UnlockShared: {
			if (s.excl > 0 && s.under > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				--s.under;
			}
			else if (s.shared > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				--s.shared;
			}
			else
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           true, std::errc::operation_not_permitted, s, slot, iter, m, op);
				result = 2;
			}
			break;
		}
		case Op::Held: {
			const bool held = mtx.is_held_exclusive();
			if (held != (s.excl > 0))
			{
				fail(opName(op), slot, iter, s, m);
			}
			result = held ? 0 : 1;
			break;
		}
		default:
			break;
		}
		record(slot, iter, op, m, s, result);
	}

	// ---------------------------------------------------------------------
	// Phase 2: one mutex per thread at a time. Local invariants + global ME.
	// ---------------------------------------------------------------------
	void applyConcurrent(Mutex& mtx, Shadow& s, Global& g, int m, Op op, int slot, std::uint32_t iter)
	{
		std::uint8_t result = 0;
		switch (op)
		{
		case Op::Lock: {
			if (s.shared > 0)
			{
				expectCall(attempt([&] {
					           mtx.lock();
					           return true;
				           }),
				           true, std::errc::resource_deadlock_would_occur, s, slot, iter, m, op);
				result = 2;
			}
			else
			{
				// May block behind the current owner; it must come back held.
				expectCall(attempt([&] {
					           mtx.lock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				if (s.excl == 0)
				{
					enterExclusive(g, m);
				}
				++s.excl;
			}
			break;
		}
		case Op::TryLock: {
			if (s.excl > 0)
			{
				const Attempt a = attempt([&] { return mtx.try_lock(); });
				if (a.threw || !a.value)
				{
					fail(opName(op), slot, iter, s, m);
				}
				++s.excl;
			}
			else if (s.shared > 0)
			{
				const Attempt a = attempt([&] { return mtx.try_lock(); });
				if (a.threw || a.value)
				{
					fail(opName(op), slot, iter, s, m);
				}
				result = 1;
			}
			else
			{
				// Contended: either outcome is legal; success updates the model.
				const Attempt a = attempt([&] { return mtx.try_lock(); });
				if (a.threw)
				{
					fail(opName(op), slot, iter, s, m);
				}
				if (a.value)
				{
					enterExclusive(g, m);
					s.excl = 1;
				}
				else
				{
					result = 1;
				}
			}
			break;
		}
		case Op::Unlock: {
			if (s.excl <= 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           true, std::errc::operation_not_permitted, s, slot, iter, m, op);
				result = 2;
			}
			else if (s.excl > 1)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				--s.excl;
			}
			else if (s.under > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           true, std::errc::operation_not_permitted, s, slot, iter, m, op);
				result = 2;
			}
			else
			{
				// Decrement before releasing: the lock must not be handed over
				// while this thread still counts as inside.
				leaveExclusive(g, m);
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				s.excl = 0;
			}
			break;
		}
		case Op::LockShared: {
			if (s.excl > 0)
			{
				expectCall(attempt([&] {
					           mtx.lock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				++s.under;
			}
			else
			{
				expectCall(attempt([&] {
					           mtx.lock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				if (s.shared == 0)
				{
					enterShared(g, m);
				}
				++s.shared;
			}
			break;
		}
		case Op::TryLockShared: {
			const Attempt a = attempt([&] { return mtx.try_lock_shared(); });
			if (a.threw)
			{
				fail(opName(op), slot, iter, s, m);
			}
			if (s.excl > 0 || s.shared > 0)
			{
				if (!a.value)
				{
					fail(opName(op), slot, iter, s, m);
				}
				if (s.excl > 0)
				{
					++s.under;
				}
				else
				{
					++s.shared;
				}
			}
			else if (a.value)
			{
				enterShared(g, m);
				s.shared = 1;
			}
			else
			{
				result = 1;
			}
			break;
		}
		case Op::UnlockShared: {
			if (s.excl > 0 && s.under > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				--s.under;
			}
			else if (s.shared > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           false, {}, s, slot, iter, m, op);
				--s.shared;
			}
			else
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           true, std::errc::operation_not_permitted, s, slot, iter, m, op);
				result = 2;
			}
			break;
		}
		case Op::Held: {
			const bool held = mtx.is_held_exclusive();
			if (held != (s.excl > 0))
			{
				fail(opName(op), slot, iter, s, m);
			}
			result = held ? 0 : 1;
			break;
		}
		default:
			break;
		}
		record(slot, iter, op, m, s, result);
	}

	struct Options
	{
		std::uint64_t seed = 0; // 0 = derive from the clock and print it
		long long ops = 1000000;
		int threads = 4;
		int seconds = 5;
		int mutexes = 8;
	};

	Options parse(int argc, char** argv)
	{
		Options o;
		for (int i = 1; i + 1 < argc; ++i)
		{
			const char* a = argv[i];
			const char* v = argv[i + 1];
			if (std::strcmp(a, "--seed") == 0)
			{
				o.seed = std::strtoull(v, nullptr, 0);
			}
			else if (std::strcmp(a, "--ops") == 0)
			{
				o.ops = std::atoll(v);
			}
			else if (std::strcmp(a, "--threads") == 0)
			{
				o.threads = std::atoi(v);
			}
			else if (std::strcmp(a, "--seconds") == 0)
			{
				o.seconds = std::atoi(v);
			}
			else if (std::strcmp(a, "--mutexes") == 0)
			{
				o.mutexes = std::atoi(v);
			}
		}
		if (o.ops < 0 || o.threads < 1 || o.threads > kMaxThreads || o.mutexes < 1 || o.seconds < 0)
		{
			std::printf("usage: model_check [--seed N] [--ops N>=0] [--threads 1..%d] [--seconds S>=0] [--mutexes K>=1]\n", kMaxThreads);
			std::exit(2);
		}
		return o;
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

} // namespace

int main(int argc, char** argv)
{
	Options opt = parse(argc, argv);
	if (opt.seed == 0)
	{
		opt.seed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0xC0FFEEULL;
	}
	g_seed = opt.seed;

	std::printf("rsm::RecursiveSharedMutex model check\n  toolchain: %s\n  seed=%llu mutexes=%d\n", toolchain().c_str(),
	            static_cast<unsigned long long>(opt.seed), opt.mutexes);

	const auto mutexes = [&] {
		std::vector<std::unique_ptr<Mutex>> v;
		v.reserve(static_cast<std::size_t>(opt.mutexes));
		for (int i = 0; i < opt.mutexes; ++i)
		{
			v.push_back(std::make_unique<Mutex>());
		}
		return v;
	}();

	// The watchdog covers both phases: a self-deadlock in phase 1 (a mutant that
	// routes a shared lock into the backend while holding the exclusive one, for
	// example) must not hang the process - it must report the stall.
	std::atomic<bool> done{false};
	std::thread watchdog([&] {
		std::uint64_t last = 0;
		int idle = 0;
		while (!done.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
			const std::uint64_t now = g_progress.load(std::memory_order_relaxed);
			if (now == last)
			{
				if (++idle >= 10)
				{
					std::fprintf(stderr, "\nMODEL CHECK STALLED: no progress for 10s (deadlock?)\n");
					dumpRings(stderr);
					std::_Exit(3);
				}
			}
			else
			{
				idle = 0;
				last = now;
			}
		}
	});

	// ---- Phase 1: single thread, exact expectations -------------------------
	{
		std::mt19937_64 rng(opt.seed);
		std::vector<Shadow> model(static_cast<std::size_t>(opt.mutexes));
		std::uint64_t digest = 1469598103934665603ULL;
		for (long long i = 0; i < opt.ops; ++i)
		{
			const int m = static_cast<int>(rng() % static_cast<std::uint64_t>(opt.mutexes));
			const Op op = chooseOp(model[static_cast<std::size_t>(m)], rng);
			applySingle(mutexes, model, m, op, 0, static_cast<std::uint32_t>(i), digest);
			g_progress.fetch_add(1, std::memory_order_relaxed);
		}
		// The shadow must be empty at the end of a balanced run only if the ops
		// balanced; instead, drain whatever is still held so the concurrent phase
		// starts clean, and verify the drain matches the model.
		for (int m = 0; m < opt.mutexes; ++m)
		{
			Shadow& s = model[static_cast<std::size_t>(m)];
			Mutex& mtx = *mutexes[static_cast<std::size_t>(m)];
			while (s.under > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           false, {}, s, 0, 0xFFFFFFFFu, m, Op::UnlockShared);
				--s.under;
			}
			while (s.shared > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock_shared();
					           return true;
				           }),
				           false, {}, s, 0, 0xFFFFFFFFu, m, Op::UnlockShared);
				--s.shared;
			}
			while (s.excl > 0)
			{
				expectCall(attempt([&] {
					           mtx.unlock();
					           return true;
				           }),
				           false, {}, s, 0, 0xFFFFFFFFu, m, Op::Unlock);
				--s.excl;
			}
		}
		std::printf("  phase 1: %lld ops, digest=%016llx\n", opt.ops, static_cast<unsigned long long>(digest));
	}

	// ---- Phase 2: concurrent, local invariants + per-mutex ME -------------
	if (opt.seconds > 0 && opt.threads > 0)
	{
		Global g(opt.mutexes);
		const int threads = opt.threads;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt.seconds);

		std::vector<std::thread> workers;
		workers.reserve(static_cast<std::size_t>(threads));
		for (int t = 0; t < threads; ++t)
		{
			workers.emplace_back([&, t] {
				std::mt19937_64 rng(opt.seed + kSeedMix * static_cast<std::uint64_t>(t + 1));
				Shadow s;
				int current = static_cast<int>(rng() % static_cast<std::uint64_t>(opt.mutexes));
				for (std::uint32_t i = 0; std::chrono::steady_clock::now() < deadline; ++i)
				{
					if (s.excl == 0 && s.shared == 0 && s.under == 0)
					{
						current = static_cast<int>(rng() % static_cast<std::uint64_t>(opt.mutexes));
					}
					const Op op = chooseOp(s, rng);
					applyConcurrent(*mutexes[static_cast<std::size_t>(current)], s, g, current, op, t, i);
					g_progress.fetch_add(1, std::memory_order_relaxed);
					if (const auto r = rng() % 64; r == 0)
					{
						std::this_thread::yield();
					}
				}
				// Each worker must be fully released: the drain below would fail the
				// model otherwise, and a stranded lock would hang the join.
				while (s.under > 0)
				{
					applyConcurrent(*mutexes[static_cast<std::size_t>(current)], s, g, current, Op::UnlockShared, t, 0xFFFFFFFFu);
				}
				while (s.shared > 0)
				{
					applyConcurrent(*mutexes[static_cast<std::size_t>(current)], s, g, current, Op::UnlockShared, t, 0xFFFFFFFFu);
				}
				while (s.excl > 0)
				{
					applyConcurrent(*mutexes[static_cast<std::size_t>(current)], s, g, current, Op::Unlock, t, 0xFFFFFFFFu);
				}
			});
		}
		for (auto& w : workers)
		{
			w.join();
		}

		// Quiescent check: nothing may be left held on any mutex.
		bool stranded = false;
		for (const auto& m : mutexes)
		{
			if (!m->try_lock())
			{
				stranded = true;
			}
			else
			{
				m->unlock();
			}
			if (!m->try_lock_shared())
			{
				stranded = true;
			}
			else
			{
				m->unlock_shared();
			}
		}

		int insideTotal = 0;
		for (const auto& a : g.inside)
		{
			insideTotal += a.load();
		}
		std::printf(
		    "  phase 2: %d threads x %ds, ops=%llu entries=%lld exits=%lld inside=%d overlaps=%lld readerWithWriter=%lld stranded=%s\n",
		    threads, opt.seconds, static_cast<unsigned long long>(g_progress.load()), g.exclusiveEntered.load(), g.exclusiveLeft.load(),
		    insideTotal, g.overlaps.load(), g.readerWithWriter.load(), stranded ? "YES" : "no");

		if (g.overlaps.load() != 0 || g.readerWithWriter.load() != 0 || stranded)
		{
			std::fprintf(stderr, "\nMODEL CHECK FAILED: global invariants\n");
			dumpRings(stderr);
			done.store(true, std::memory_order_release);
			watchdog.join();
			return 1;
		}
	}

	done.store(true, std::memory_order_release);
	watchdog.join();

	std::printf("VERDICT: model holds (seed %llu).\n", static_cast<unsigned long long>(opt.seed));
	return 0;
}
