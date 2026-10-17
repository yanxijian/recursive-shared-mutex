// R2 observability tests for the instrumented write guard.
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <recursive_shared_mutex/instrumented_write_lock.hpp>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <stdexcept>
#include <system_error>
#include <thread>

using rsm::RecursiveSharedMutex;
using rsm::metrics::AtomicStatsRecorder;
using rsm::metrics::instrumented_write_lock;
using rsm::metrics::Recorder;
using rsm::metrics::set_recorder;

namespace
{
	constexpr std::uint64_t kNs = 1000000000ULL; // 1 second in nanoseconds
	constexpr std::uint64_t kMs = 1000000ULL;    // 1 millisecond in nanoseconds

	// Function-local static: outlives every guard, per the header contract.
	AtomicStatsRecorder& sharedStats()
	{
		static AtomicStatsRecorder recorder;
		return recorder;
	}

	struct InstallSink
	{
		explicit InstallSink(AtomicStatsRecorder& recorder)
		{
			set_recorder(&recorder);
		}
		~InstallSink()
		{
			set_recorder(nullptr);
		}
	};

	class ThrowingRecorder final : public Recorder
	{
	public:
		void observe_write_wait(double) override
		{
			throw std::runtime_error("sink boom");
		}
		void observe_write_hold(double) override
		{
			throw std::runtime_error("sink boom");
		}
	};
} // namespace

TEST_CASE("hold is measured for the exclusive critical section", "[metrics][M1]")
{
	InstallSink install(sharedStats());
	RecursiveSharedMutex mtx;
	const auto before = sharedStats().snapshot();
	{
		instrumented_write_lock guard(mtx);
		REQUIRE(guard.owns_lock());
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	const auto after = sharedStats().snapshot();

	REQUIRE(after.hold_count - before.hold_count == 1);
	REQUIRE(after.wait_count - before.wait_count == 1);
	REQUIRE(after.hold_total_ns - before.hold_total_ns >= 90 * kMs); // >= 90ms
	REQUIRE(after.hold_max_ns >= 90 * kMs);
	REQUIRE(after.wait_total_ns - before.wait_total_ns < 90 * kMs); // uncontended: tiny wait
}

TEST_CASE("wait includes the time blocked on a foreign exclusive holder", "[metrics][M2]")
{
	InstallSink install(sharedStats());
	RecursiveSharedMutex mtx;
	std::atomic<bool> holderLocked{false};
	std::atomic<bool> release{false};
	std::atomic<bool> acquired{false};

	std::thread holder([&]() {
		mtx.lock();
		holderLocked = true;
		while (!release.load())
		{
			std::this_thread::yield();
		}
		mtx.unlock();
	});
	while (!holderLocked.load())
	{
		std::this_thread::yield();
	}

	const auto before = sharedStats().snapshot();
	std::thread writer([&]() {
		instrumented_write_lock guard(mtx); // blocks ~120ms until holder releases
		acquired = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(120)); // writer blocked meanwhile
	release = true;                                              // holder unlocks -> writer acquires (wait ~= 120ms)
	writer.join();
	REQUIRE(acquired.load());
	const auto after = sharedStats().snapshot();

	REQUIRE(after.wait_count - before.wait_count == 1);
	REQUIRE(after.wait_total_ns - before.wait_total_ns >= 100 * kMs); // >= 100ms
	REQUIRE(after.hold_count - before.hold_count == 1);

	holder.join();
}

TEST_CASE("nested write reentrancy reports overlapping per-guard holds", "[metrics][M3]")
{
	InstallSink install(sharedStats());
	RecursiveSharedMutex mtx;
	const auto before = sharedStats().snapshot();
	{
		instrumented_write_lock outer(mtx);
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		{
			instrumented_write_lock inner(mtx); // reentrant fast path: wait ~ 0
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const auto after = sharedStats().snapshot();

	REQUIRE(after.hold_count - before.hold_count == 2);
	REQUIRE(after.wait_count - before.wait_count == 2);
	REQUIRE(after.hold_total_ns - before.hold_total_ns >= 90 * kMs); // 60ms + 30ms
	// Inner guard's wait (reentrant fast path) must be far below 30ms.
	REQUIRE(after.wait_total_ns - before.wait_total_ns < 30 * kMs);
}

TEST_CASE("fail-fast construction records nothing", "[metrics][M4]")
{
	InstallSink install(sharedStats());
	RecursiveSharedMutex mtx;
	const auto before = sharedStats().snapshot();
	{
		std::shared_lock<RecursiveSharedMutex> rl(mtx);
		bool threw = false;
		try
		{
			instrumented_write_lock guard(mtx); // read-then-write misuse
			(void)guard;
		}
		catch (const std::system_error& e)
		{
			threw = e.code() == std::make_error_code(std::errc::resource_deadlock_would_occur);
		}
		REQUIRE(threw);
	} // rl releases cleanly
	const auto after = sharedStats().snapshot();

	REQUIRE(after.wait_count == before.wait_count);
	REQUIRE(after.hold_count == before.hold_count);
	REQUIRE(after.wait_total_ns == before.wait_total_ns);
	REQUIRE(after.hold_total_ns == before.hold_total_ns);
}

TEST_CASE("guard works without a recorder installed", "[metrics][M5]")
{
	set_recorder(nullptr);
	RecursiveSharedMutex mtx;
	{
		instrumented_write_lock guard(mtx);
		REQUIRE(guard.owns_lock());
	}
	REQUIRE(mtx.try_lock());
	mtx.unlock();
}

TEST_CASE("recorder exceptions are swallowed by the guard", "[metrics][M6]")
{
	static ThrowingRecorder throwing;
	set_recorder(&throwing);
	RecursiveSharedMutex mtx;
	{
		instrumented_write_lock guard(mtx); // reports will throw; must not escape
		REQUIRE(guard.owns_lock());
	}
	REQUIRE(mtx.try_lock());
	mtx.unlock();
	set_recorder(nullptr);
}

TEST_CASE("concurrent instrumented writers count exactly", "[metrics][M7]")
{
	InstallSink install(sharedStats());
	RecursiveSharedMutex mtx;
	std::atomic<int> value{0};
	const auto before = sharedStats().snapshot();
	constexpr int kThreads = 4;
	constexpr int kIters = 50;

	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back([&]() {
			for (int i = 0; i < kIters; ++i)
			{
				instrumented_write_lock guard(mtx);
				value.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	for (auto& t : threads)
	{
		t.join();
	}

	const auto after = sharedStats().snapshot();
	REQUIRE(value.load() == kThreads * kIters);
	REQUIRE(after.hold_count - before.hold_count == kThreads * kIters);
	REQUIRE(after.wait_count - before.wait_count == kThreads * kIters);
}

TEST_CASE("hold is reported after the release, never inside the critical section", "[metrics][M8]")
{
	// The Recorder contract says a sink is never called while the lock is held, so
	// user code there cannot end up inside the critical section. Prove it from a
	// second thread: while HOLD is being reported, the mutex must be acquirable.
	RecursiveSharedMutex mtx;
	std::atomic<bool> reporting{false};
	std::atomic<bool> go{false};
	std::atomic<bool> stop{false};
	std::atomic<bool> freed{false};

	struct WindowRecorder final : Recorder
	{
		explicit WindowRecorder(std::atomic<bool>& flag)
		    : reporting_(flag)
		{
		}
		void observe_write_wait(double) override
		{
		}
		void observe_write_hold(double) override
		{
			reporting_.store(true, std::memory_order_release);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
			while (std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			reporting_.store(false, std::memory_order_release);
		}
		std::atomic<bool>& reporting_;
	} recorder(reporting);

	std::thread prober([&]() {
		while (!go.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		while (!stop.load(std::memory_order_acquire))
		{
			if (mtx.try_lock())
			{
				if (reporting.load(std::memory_order_acquire))
				{
					freed.store(true, std::memory_order_release);
				}
				mtx.unlock();
			}
			std::this_thread::yield();
		}
	});

	set_recorder(&recorder);
	{
		instrumented_write_lock guard(mtx);
		go.store(true, std::memory_order_release); // prober starts while the guard is held
	}
	stop.store(true, std::memory_order_release);
	prober.join();
	set_recorder(nullptr);

	REQUIRE(freed.load());
}
