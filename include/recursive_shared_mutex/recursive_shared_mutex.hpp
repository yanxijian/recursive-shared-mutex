#pragma once

#include <atomic>
#include <shared_mutex>
#include <system_error>
#include <thread>

// C++17 header-only recursive shared_mutex.
//
// Exclusive ownership lives on the instance (`atomic` owner id + owner-only counters).
// Shared-recursion depth is per (thread × mutex) via a thread_local intrusive list.
// Note: thread_local std::unordered_map has exhibited heap corruption under shared_mutex
// contention on some MinGW libstdc++ builds; a simple list avoids that allocator path.
//
// Rules:
// - Same thread may re-enter lock() / lock_shared().
// - Holding exclusive lock, lock_shared() is allowed (under_exclusive_).
// - Holding shared lock, lock() is FORBIDDEN (upgrade deadlock) -> system_error.
// - lock/unlock must stay on the same OS thread (no cross-thread unlock).
// - Destroying a mutex while any thread still holds it is UB (same as std::mutex).

namespace rsm
{

	class RecursiveSharedMutex
	{
	public:
		RecursiveSharedMutex() = default;
		~RecursiveSharedMutex() = default;

		RecursiveSharedMutex(const RecursiveSharedMutex&) = delete;
		RecursiveSharedMutex& operator=(const RecursiveSharedMutex&) = delete;
		RecursiveSharedMutex(RecursiveSharedMutex&&) = delete;
		RecursiveSharedMutex& operator=(RecursiveSharedMutex&&) = delete;

		void lock()
		{
			const std::thread::id self = std::this_thread::get_id();
			if (owner_.load(std::memory_order_relaxed) == self)
			{
				++exclusive_count_;
				return;
			}
			if (sharedDepthValue() > 0)
			{
				throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
				                        "RecursiveSharedMutex: cannot upgrade shared lock to exclusive");
			}
			mtx_.lock();
			owner_.store(self, std::memory_order_relaxed);
			exclusive_count_ = 1;
		}

		bool try_lock()
		{
			const std::thread::id self = std::this_thread::get_id();
			if (owner_.load(std::memory_order_relaxed) == self)
			{
				++exclusive_count_;
				return true;
			}
			if (sharedDepthValue() > 0)
			{
				return false;
			}
			if (!mtx_.try_lock())
			{
				return false;
			}
			owner_.store(self, std::memory_order_relaxed);
			exclusive_count_ = 1;
			return true;
		}

		void unlock()
		{
			const std::thread::id self = std::this_thread::get_id();
			if (owner_.load(std::memory_order_relaxed) != self || exclusive_count_ <= 0)
			{
				throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
				                        "RecursiveSharedMutex::unlock without exclusive ownership");
			}
			if (exclusive_count_ > 1)
			{
				--exclusive_count_;
				return;
			}
			if (under_exclusive_ != 0)
			{
				throw std::system_error(
				    std::make_error_code(std::errc::operation_not_permitted),
				    "RecursiveSharedMutex::unlock while shared_lock still nested under unique_lock");
			}
			exclusive_count_ = 0;
			owner_.store(std::thread::id(), std::memory_order_relaxed);
			mtx_.unlock();
		}

		void lock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id())
			{
				++under_exclusive_;
				return;
			}

			int* depth = findSharedDepth();
			if (depth != nullptr && *depth > 0)
			{
				++(*depth);
				return;
			}

			mtx_.lock_shared();
			ensureSharedDepth() = 1;
		}

		bool try_lock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id())
			{
				++under_exclusive_;
				return true;
			}

			int* depth = findSharedDepth();
			if (depth != nullptr && *depth > 0)
			{
				++(*depth);
				return true;
			}

			if (!mtx_.try_lock_shared())
			{
				return false;
			}
			ensureSharedDepth() = 1;
			return true;
		}

		void unlock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id() && under_exclusive_ > 0)
			{
				--under_exclusive_;
				return;
			}

			int* depth = findSharedDepth();
			if (depth == nullptr || *depth <= 0)
			{
				throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
				                        "RecursiveSharedMutex::unlock_shared without shared ownership");
			}
			if (*depth > 1)
			{
				--(*depth);
				return;
			}
			*depth = 0;
			mtx_.unlock_shared();
		}

	private:
		struct SharedNode
		{
			const RecursiveSharedMutex* key;
			int depth;
			SharedNode* next;
		};

		static SharedNode*& head()
		{
			thread_local SharedNode* h = nullptr;
			return h;
		}

		int* findSharedDepth() const
		{
			for (SharedNode* node = head(); node != nullptr; node = node->next)
			{
				if (node->key == this)
				{
					return &node->depth;
				}
			}
			return nullptr;
		}

		int& ensureSharedDepth()
		{
			if (int* existing = findSharedDepth())
			{
				return *existing;
			}
			SharedNode* node = new SharedNode{this, 0, head()};
			head() = node;
			return node->depth;
		}

		int sharedDepthValue() const
		{
			const int* depth = findSharedDepth();
			return depth == nullptr ? 0 : *depth;
		}

		std::shared_mutex mtx_;
		std::atomic<std::thread::id> owner_{};
		int exclusive_count_ = 0;
		int under_exclusive_ = 0;
	};

} // namespace rsm
