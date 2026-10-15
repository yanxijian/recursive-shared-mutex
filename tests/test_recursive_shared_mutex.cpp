#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
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
