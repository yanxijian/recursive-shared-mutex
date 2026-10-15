#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <shared_mutex>
#include <system_error>
#include <thread>
#include <vector>

using rsm::RecursiveSharedMutex;

// --- Global allocation hook (E1): one-shot bad_alloc injection --------------
// Replacing global operator new lets the TLS node allocation inside
// lock_shared()/try_lock_shared() fail deterministically. The flag is
// one-shot: the first allocation after it is set throws, then it clears.
namespace
{
	std::atomic<bool> g_failNextAlloc{false};
}

void* operator new(std::size_t n)
{
	if (g_failNextAlloc.exchange(false, std::memory_order_acq_rel))
	{
		throw std::bad_alloc();
	}
	void* p = std::malloc(n);
	if (p == nullptr)
	{
		throw std::bad_alloc();
	}
	return p;
}

void* operator new[](std::size_t n)
{
	return ::operator new(n);
}

void operator delete(void* p) noexcept
{
	std::free(p);
}

void operator delete[](void* p) noexcept
{
	std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
	std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
	std::free(p);
}

TEST_CASE("exclusive lock reentrancy", "[single][T1]")
{
	RecursiveSharedMutex mtx;
	mtx.lock();
	mtx.lock();
	mtx.lock();
	mtx.unlock();
	mtx.unlock();
	mtx.unlock();
	REQUIRE(mtx.try_lock());
	mtx.unlock();
}

TEST_CASE("shared lock reentrancy", "[single][T2]")
{
	RecursiveSharedMutex mtx;
	mtx.lock_shared();
	mtx.lock_shared();
	mtx.unlock_shared();
	mtx.unlock_shared();
	REQUIRE(mtx.try_lock_shared());
	mtx.unlock_shared();
}

TEST_CASE("unique_lock nests shared_lock", "[single][T3]")
{
	RecursiveSharedMutex mtx;
	{
		std::unique_lock<RecursiveSharedMutex> exclusive(mtx);
		{
			std::shared_lock<RecursiveSharedMutex> shared(mtx);
			REQUIRE(shared.owns_lock());
		}
		REQUIRE(exclusive.owns_lock());
	}
	REQUIRE(mtx.try_lock());
	mtx.unlock();
}

TEST_CASE("shared then exclusive throws deadlock error", "[single][T4]")
{
	RecursiveSharedMutex mtx;
	mtx.lock_shared();
	REQUIRE_THROWS_AS(mtx.lock(), std::system_error);
	try
	{
		mtx.lock();
	}
	catch (const std::system_error& ex)
	{
		REQUIRE(ex.code() == std::errc::resource_deadlock_would_occur);
	}
	mtx.unlock_shared();
}

TEST_CASE("unlock without ownership throws", "[single][T5]")
{
	RecursiveSharedMutex mtx;
	REQUIRE_THROWS_AS(mtx.unlock(), std::system_error);
	REQUIRE_THROWS_AS(mtx.unlock_shared(), std::system_error);
}

TEST_CASE("unlock exclusive while nested shared still held throws", "[single][T6]")
{
	RecursiveSharedMutex mtx;
	mtx.lock();
	mtx.lock_shared();
	REQUIRE_THROWS_AS(mtx.unlock(), std::system_error);
	mtx.unlock_shared();
	mtx.unlock();
}

TEST_CASE("try_lock while shared held returns false", "[single][T7]")
{
	RecursiveSharedMutex mtx;
	mtx.lock_shared();
	REQUIRE_FALSE(mtx.try_lock());
	mtx.unlock_shared();
}

TEST_CASE("try_lock and try_lock_shared while exclusive held", "[single][T8]")
{
	RecursiveSharedMutex mtx;
	REQUIRE(mtx.try_lock());
	REQUIRE(mtx.try_lock());
	REQUIRE(mtx.try_lock_shared());
	mtx.unlock_shared();
	mtx.unlock();
	mtx.unlock();
}

TEST_CASE("multiple mutex instances do not share recursion counts", "[multi][M1][M2]")
{
	RecursiveSharedMutex a;
	RecursiveSharedMutex b;

	a.lock();

	std::atomic<bool> other_failed_on_a{false};
	std::atomic<bool> other_got_b{false};
	std::thread probe([&]() {
		other_failed_on_a = !a.try_lock();
		other_got_b = b.try_lock();
		if (other_got_b)
		{
			b.unlock();
		}
	});
	probe.join();

	REQUIRE(other_failed_on_a);
	REQUIRE(other_got_b);

	// Holding a must not fake-reenter b
	b.lock();
	b.unlock();
	a.unlock();
}

TEST_CASE("concurrent readers and one writer", "[concurrent][C1][C2]")
{
	RecursiveSharedMutex mtx;
	std::atomic<int> value{0};
	std::atomic<int> readers_ok{0};
	constexpr int kReaders = 8;
	constexpr int kIters = 200;

	std::thread writer([&]() {
		for (int i = 0; i < kIters; ++i)
		{
			std::unique_lock<RecursiveSharedMutex> lock(mtx);
			value = i;
			std::this_thread::sleep_for(std::chrono::microseconds(50));
			value = i;
		}
	});

	std::vector<std::thread> readers;
	readers.reserve(kReaders);
	for (int r = 0; r < kReaders; ++r)
	{
		readers.emplace_back([&]() {
			for (int i = 0; i < kIters; ++i)
			{
				std::shared_lock<RecursiveSharedMutex> lock(mtx);
				const int snapshot = value.load(std::memory_order_relaxed);
				(void)snapshot;
				readers_ok.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	writer.join();
	for (auto& t : readers)
	{
		t.join();
	}

	REQUIRE(readers_ok == kReaders * kIters);
}

TEST_CASE("try paths against foreign holders", "[try][P1][P2]")
{
	RecursiveSharedMutex mtx;
	std::atomic<bool> ready{false};
	std::atomic<bool> done{false};

	std::thread holder([&]() {
		mtx.lock();
		ready = true;
		while (!done.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		mtx.unlock();
	});

	while (!ready.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	REQUIRE_FALSE(mtx.try_lock());
	REQUIRE_FALSE(mtx.try_lock_shared());
	done = true;
	holder.join();

	mtx.lock_shared();
	std::thread exclusive_probe([&]() { REQUIRE_FALSE(mtx.try_lock()); });
	exclusive_probe.join();
	REQUIRE(mtx.try_lock_shared());
	mtx.unlock_shared();
	mtx.unlock_shared();
}

TEST_CASE("lock_shared allocation failure does not strand the shared slot", "[exception][E1]")
{
	RecursiveSharedMutex mtx;

	g_failNextAlloc.store(true, std::memory_order_release);
	bool threw = false;
	try
	{
		mtx.lock_shared();
	}
	catch (const std::bad_alloc&)
	{
		threw = true;
	}
	REQUIRE(threw);

	// The underlying shared slot must not be held: another thread can take
	// and release the exclusive lock right away.
	std::atomic<bool> gotExclusive{false};
	std::thread probe([&]() {
		gotExclusive = mtx.try_lock();
		if (gotExclusive.load(std::memory_order_relaxed))
		{
			mtx.unlock();
		}
	});
	probe.join();
	REQUIRE(gotExclusive.load());

	// Still usable afterwards.
	mtx.lock_shared();
	mtx.unlock_shared();
}

TEST_CASE("try_lock_shared failure leaves no TLS node behind", "[exception][E2]")
{
	RecursiveSharedMutex mtx;
	std::atomic<bool> ready{false};
	std::atomic<bool> done{false};

	std::thread holder([&]() {
		mtx.lock();
		ready = true;
		while (!done.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		mtx.unlock();
	});
	while (!ready.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	const std::size_t before = RecursiveSharedMutex::live_shared_nodes.load();
	REQUIRE_FALSE(mtx.try_lock_shared());
	REQUIRE(RecursiveSharedMutex::live_shared_nodes.load() == before);

	done = true;
	holder.join();
}

TEST_CASE("thread exit releases its TLS shared nodes", "[tls][E3]")
{
	const std::size_t before = RecursiveSharedMutex::live_shared_nodes.load();

	std::thread worker([]() {
		for (int i = 0; i < 8; ++i)
		{
			RecursiveSharedMutex mtx;
			mtx.lock_shared();
			mtx.unlock_shared();
		}
	});
	worker.join();

	REQUIRE(RecursiveSharedMutex::live_shared_nodes.load() == before);
}

TEST_CASE("move-to-front keeps hot mutex TLS lookups O(1)", "[tls][E4]")
{
	RecursiveSharedMutex a;
	RecursiveSharedMutex b;

	// Build list order [b, a]: touch a first, then b.
	a.lock_shared();
	a.unlock_shared();
	b.lock_shared();
	b.unlock_shared();

	// First lookup of a moves it to the front of the list.
	a.lock_shared();
	a.unlock_shared();

	const std::uint64_t stepsBefore = RecursiveSharedMutex::shared_scan_steps.load();
	constexpr int kCycles = 8;
	for (int i = 0; i < kCycles; ++i)
	{
		a.lock_shared();
		a.unlock_shared();
	}
	const std::uint64_t stepsAfter = RecursiveSharedMutex::shared_scan_steps.load();

	// One find per lock_shared/unlock_shared, each visiting exactly one node.
	REQUIRE(stepsAfter - stepsBefore == 2 * static_cast<std::uint64_t>(kCycles));
}
