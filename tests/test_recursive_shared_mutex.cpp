#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <shared_mutex>
#include <system_error>
#include <thread>
#include <vector>

using rsm::RecursiveSharedMutex;

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

TEST_CASE("exclusive path stays flat while many shared locks are held", "[perf][T12]")
{
	// Holding shared locks on other mutexes must not slow the exclusive path down.
	// The upgrade guard needs a slot lookup, and that lookup is linear in how many
	// shared locks the thread holds; this test is the regression gate for keeping
	// the scan off the common path (measured: 4.6 -> 30 ns/op at 64 held slots
	// before the try_lock-first restructuring, 5-6 ns/op after).
	constexpr int kHeld = 64;
	constexpr int kIterations = 20000;

	std::vector<RecursiveSharedMutex> held(kHeld);
	RecursiveSharedMutex probe;

	auto measure = [&probe](int iterations) {
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < iterations; ++i)
		{
			probe.lock();
			probe.unlock();
		}
		const auto done = std::chrono::steady_clock::now();
		return std::chrono::duration<double, std::nano>(done - start).count() / iterations;
	};

	measure(kIterations / 4); // warm up
	const double withNoneHeld = measure(kIterations);

	for (auto& m : held)
	{
		m.lock_shared();
	}
	const double withManyHeld = measure(kIterations);

	// Behavioural part: the exclusive path still works, and an upgrade attempt on a
	// mutex this thread holds shared still throws rather than deadlocking.
	REQUIRE(held.front().try_lock_shared()); // recursive shared under a shared lock
	held.front().unlock_shared();
	bool upgraded = false;
	try
	{
		held.front().lock();
	}
	catch (const std::system_error&)
	{
		upgraded = true;
	}
	REQUIRE(upgraded);

	for (auto& m : held)
	{
		m.unlock_shared();
	}

	// Timing part is deliberately loose (shared CI runners are noisy) but still
	// catches the old linear scan, which made this ratio 6x.
	INFO("with none held: " << withNoneHeld << " ns/op, with " << kHeld << " held: " << withManyHeld << " ns/op");
	REQUIRE(withManyHeld < withNoneHeld * 4.0);
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
	// Assertions must run on the main thread: a failing REQUIRE inside another
	// thread escapes as Catch::TestFailureException and terminates the process
	// instead of failing the test case. Record the observation, assert here.
	bool probeAcquired = true;
	std::thread exclusive_probe([&]() {
		probeAcquired = mtx.try_lock();
		if (probeAcquired)
		{
			mtx.unlock();
		}
	});
	exclusive_probe.join();
	REQUIRE_FALSE(probeAcquired);
	REQUIRE(mtx.try_lock_shared());
	mtx.unlock_shared();
	mtx.unlock_shared();
}

TEST_CASE("lock_shared acquire failure does not strand the shared slot", "[exception][E1]")
{
	RecursiveSharedMutex mtx;

	RecursiveSharedMutex::fail_next_shared_acquire.store(1, std::memory_order_release);
	bool threw = false;
	try
	{
		mtx.lock_shared();
	}
	catch (const std::system_error& ex)
	{
		threw = ex.code() == std::errc::resource_unavailable_try_again;
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
	RecursiveSharedMutex::fail_next_shared_acquire.store(0, std::memory_order_release);
	mtx.lock_shared();
	mtx.unlock_shared();
}

TEST_CASE("try_lock_shared contention fails cleanly and stays usable", "[exception][E2]")
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

	REQUIRE_FALSE(mtx.try_lock_shared());

	done = true;
	holder.join();

	// Usable once the exclusive holder is gone.
	REQUIRE(mtx.try_lock_shared());
	mtx.unlock_shared();
}

TEST_CASE("shared slots are recycled after unlock and the cap is enforced", "[tls][E3]")
{
	// Recycling: touching many distinct mutexes one at a time never exhausts
	// the per-thread table (the old TLS-node design leaked one node per
	// mutex for the lifetime of the thread).
	constexpr int kMany = 4 * static_cast<int>(RecursiveSharedMutex::kSharedSlotCount);
	for (int i = 0; i < kMany; ++i)
	{
		RecursiveSharedMutex mtx;
		mtx.lock_shared();
		mtx.unlock_shared();
	}

	// Cap: holding kSharedSlotCount shared locks concurrently, one more must
	// fail fast with resource_unavailable_try_again.
	std::vector<std::unique_ptr<RecursiveSharedMutex>> held;
	held.reserve(RecursiveSharedMutex::kSharedSlotCount + 1);
	for (std::size_t i = 0; i < RecursiveSharedMutex::kSharedSlotCount; ++i)
	{
		held.push_back(std::make_unique<RecursiveSharedMutex>());
		held.back()->lock_shared();
	}
	bool threw = false;
	try
	{
		RecursiveSharedMutex extra;
		extra.lock_shared();
	}
	catch (const std::system_error& ex)
	{
		threw = ex.code() == std::errc::resource_unavailable_try_again;
	}
	REQUIRE(threw);

	// The try_* half of the same asymmetry: with the table just as full,
	// try_lock_shared() must report "could not acquire" instead of throwing.
	// R9 calls this deliberate; until now nothing exercised it.
	{
		RecursiveSharedMutex extra;
		REQUIRE_FALSE(extra.try_lock_shared());
	}

	for (auto& m : held)
	{
		m->unlock_shared();
	}
	held.clear();

	// Table is free again after releasing everything, for both acquire paths.
	RecursiveSharedMutex after;
	REQUIRE(after.try_lock_shared());
	after.unlock_shared();
}

TEST_CASE("try_lock_shared acquire failure does not strand the shared slot", "[exception][E5]")
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

	RecursiveSharedMutex::fail_next_shared_acquire.store(1, std::memory_order_release);
	bool threw = false;
	try
	{
		mtx.try_lock_shared();
	}
	catch (const std::system_error& ex)
	{
		threw = ex.code() == std::errc::resource_unavailable_try_again;
	}
	REQUIRE(threw);

	done = true;
	holder.join();

	RecursiveSharedMutex::fail_next_shared_acquire.store(0, std::memory_order_release);
	mtx.lock_shared();
	mtx.unlock_shared();
}

TEST_CASE("exclusive lock blocks behind a foreign writer while a shared lock is held elsewhere", "[concurrent][T13]")
{
	// Exercises the exclusive slow path's third branch: this thread holds a shared
	// lock on ANOTHER mutex (so its slot table is non-empty), try_lock() on the
	// contended mutex is refused, the upgrade scan finds no slot for it, and the
	// call must then block in the backend - not throw, not return early.
	RecursiveSharedMutex held_shared;
	RecursiveSharedMutex contended;
	std::atomic<bool> holder_ready{false};
	std::atomic<bool> allowed_release{false};
	std::atomic<bool> holder_released{false};

	held_shared.lock_shared(); // the calling thread's slot table is non-empty from here

	std::thread holder([&]() {
		contended.lock();
		holder_ready.store(true, std::memory_order_release);
		while (!allowed_release.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		holder_released.store(true, std::memory_order_release);
		contended.unlock();
	});

	while (!holder_ready.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	allowed_release.store(true, std::memory_order_release);

	contended.lock(); // must block until the holder releases; the release is sequenced before
	REQUIRE(holder_released.load(std::memory_order_acquire));
	contended.unlock();
	held_shared.unlock_shared();

	holder.join();

	// Nothing is stranded: both mutexes are usable again.
	REQUIRE(contended.try_lock());
	contended.unlock();
}

TEST_CASE("unlock_shared from a foreign thread is rejected and keeps the owner's slot", "[concurrent][T14]")
{
	// The migration guide calls cross-thread release the headline hazard; it must
	// fail loudly and leave the owner's accounting untouched.
	RecursiveSharedMutex mtx;
	mtx.lock_shared();

	std::atomic<bool> threw{false};
	std::atomic<bool> wrong_code{false};
	std::thread foreign([&]() {
		try
		{
			mtx.unlock_shared();
		}
		catch (const std::system_error& ex)
		{
			threw.store(true);
			wrong_code.store(ex.code() != std::errc::operation_not_permitted);
		}
	});
	foreign.join();

	REQUIRE(threw.load());
	REQUIRE_FALSE(wrong_code.load());

	mtx.unlock_shared(); // the owner's slot is intact
	REQUIRE(mtx.try_lock());
	mtx.unlock();
}
