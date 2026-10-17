// Instrumented write-path RAII guard (roadmap R2): writer-starvation
// observability for RecursiveSharedMutex.
//
// Design contracts (read before using):
//  - The core RecursiveSharedMutex is untouched; all overhead lives in this
//    opt-in guard and only on the write path.
//  - Measured quantities: write WAIT (time blocked inside lock(), from guard
//    construction to ownership) and write HOLD (exclusive critical-section
//    duration, from ownership to the release attempt). Read paths are
//    intentionally not instrumented (R2 scope).
//  - A Recorder receives the observations. Install it ONCE at startup,
//    BEFORE creating threads, via set_recorder(); the pointer must remain
//    valid for the rest of the process lifetime. Guards capture the sink
//    pointer at construction: installing or replacing the recorder later
//    only affects guards created afterwards.
//  - Recorder implementations MUST NOT throw: the guard reports from its
//    destructor and swallows everything to keep the destructor noexcept.
//  - Nested reentrant write guards each report their own wait/hold; hold
//    durations of nested scopes overlap, so histogram sums are per-guard
//    events, not exclusive wall time.
//  - The module-boundary and thread-affinity rules of the core component
//    apply unchanged; this guard does not relax any of them.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>

namespace rsm
{
	namespace metrics
	{

		class Recorder
		{
		public:
			virtual ~Recorder() = default;
			// seconds >= 0; called after the underlying unlock, never while
			// holding the lock. Implementations must not throw.
			virtual void observe_write_wait(double seconds) = 0;
			virtual void observe_write_hold(double seconds) = 0;
		};

		// Minimal built-in recorder: count / total / max per metric, atomics
		// only. snapshot() reads the counters non-atomically - treat the
		// combination as approximate under concurrent observation.
		class AtomicStatsRecorder final : public Recorder
		{
		public:
			struct Snapshot
			{
				std::uint64_t wait_count;
				std::uint64_t wait_total_ns;
				std::uint64_t wait_max_ns;
				std::uint64_t hold_count;
				std::uint64_t hold_total_ns;
				std::uint64_t hold_max_ns;
			};

			void observe_write_wait(double seconds) override
			{
				record(wait_count_, wait_total_ns_, wait_max_ns_, seconds);
			}

			void observe_write_hold(double seconds) override
			{
				record(hold_count_, hold_total_ns_, hold_max_ns_, seconds);
			}

			Snapshot snapshot() const
			{
				return {wait_count_.load(std::memory_order_relaxed),    wait_total_ns_.load(std::memory_order_relaxed),
				        wait_max_ns_.load(std::memory_order_relaxed),   hold_count_.load(std::memory_order_relaxed),
				        hold_total_ns_.load(std::memory_order_relaxed), hold_max_ns_.load(std::memory_order_relaxed)};
			}

		private:
			static void record(std::atomic<std::uint64_t>& count, std::atomic<std::uint64_t>& total, std::atomic<std::uint64_t>& maxNs,
			                   double seconds)
			{
				const auto ns = static_cast<std::uint64_t>(seconds * 1e9); // seconds >= 0
				count.fetch_add(1, std::memory_order_relaxed);
				total.fetch_add(ns, std::memory_order_relaxed);
				auto currentMax = maxNs.load(std::memory_order_relaxed);
				while (ns > currentMax && !maxNs.compare_exchange_weak(currentMax, ns, std::memory_order_relaxed))
				{
				}
			}

			std::atomic<std::uint64_t> wait_count_{0};
			std::atomic<std::uint64_t> wait_total_ns_{0};
			std::atomic<std::uint64_t> wait_max_ns_{0};
			std::atomic<std::uint64_t> hold_count_{0};
			std::atomic<std::uint64_t> hold_total_ns_{0};
			std::atomic<std::uint64_t> hold_max_ns_{0};
		};

		// Process-wide sink storage. Install once at startup; must outlive
		// all guards.
		inline std::atomic<Recorder*>& recorderSink()
		{
			static std::atomic<Recorder*> sink{nullptr};
			return sink;
		}

		inline void set_recorder(Recorder* recorder)
		{
			recorderSink().store(recorder, std::memory_order_release);
		}

		inline Recorder* active_recorder()
		{
			return recorderSink().load(std::memory_order_acquire);
		}

		// RAII write lock with wait/hold observability. Construct-acquire,
		// destructor-release only (no adopt/defer); non-copyable and
		// non-movable so the guard cannot drift across threads.
		class instrumented_write_lock
		{
		public:
			using clock = std::chrono::steady_clock;

			explicit instrumented_write_lock(RecursiveSharedMutex& mtx)
			    : mtx_(&mtx),
			      sink_(active_recorder())
			{
				if (sink_ != nullptr)
				{
					start_ = clock::now();
				}
				mtx_->lock(); // may throw (read-then-write misuse): nothing owned, nothing reported
				if (sink_ != nullptr)
				{
					acquired_ = clock::now();
				}
				owns_ = true;
			}

			~instrumented_write_lock()
			{
				if (!owns_)
				{
					return;
				}
				if (sink_ != nullptr)
				{
					report_hold(std::chrono::duration<double>(clock::now() - acquired_).count());
				}
				try
				{
					mtx_->unlock(); // accounting-consistent unlock cannot throw
				}
				catch (...)
				{
					// Only reachable if the core accounting is corrupted; the
					// underlying exclusive lock leaks - a core-component-level
					// failure. Swallowed to keep the destructor noexcept.
				}
				if (sink_ != nullptr)
				{
					report_wait(std::chrono::duration<double>(acquired_ - start_).count());
				}
			}

			instrumented_write_lock(const instrumented_write_lock&) = delete;
			instrumented_write_lock& operator=(const instrumented_write_lock&) = delete;
			instrumented_write_lock(instrumented_write_lock&&) = delete;
			instrumented_write_lock& operator=(instrumented_write_lock&&) = delete;

			bool owns_lock() const noexcept
			{
				return owns_;
			}

		private:
			void report_wait(double seconds) noexcept
			{
				try
				{
					sink_->observe_write_wait(seconds);
				}
				catch (...)
				{
				}
			}

			void report_hold(double seconds) noexcept
			{
				try
				{
					sink_->observe_write_hold(seconds);
				}
				catch (...)
				{
				}
			}

			RecursiveSharedMutex* mtx_;
			Recorder* sink_;
			clock::time_point start_{};
			clock::time_point acquired_{};
			bool owns_ = false;
		};

	} // namespace metrics
} // namespace rsm
