#pragma once

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <system_error>
#include <thread>

// C++17 header-only recursive shared_mutex.
// Exclusive: atomic owner id + owner-only counters on the instance.
// Shared recursion: thread_local intrusive list keyed by a unique instance id_
// (never reuse address-based keys across mutex lifetimes).

namespace rsm
{

	class RecursiveSharedMutex
	{
	public:
		RecursiveSharedMutex()
		    : id_(nextId().fetch_add(1, std::memory_order_relaxed))
		{
		}

		~RecursiveSharedMutex()
		{
			eraseSharedNode(id_);
		}

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
			std::uint64_t id;
			int depth;
			SharedNode* next;
		};

		static std::atomic<std::uint64_t>& nextId()
		{
			static std::atomic<std::uint64_t> n{1};
			return n;
		}

		static SharedNode*& head()
		{
			thread_local SharedNode* h = nullptr;
			return h;
		}

		static void eraseSharedNode(std::uint64_t id)
		{
			SharedNode** link = &head();
			while (*link != nullptr)
			{
				if ((*link)->id == id)
				{
					SharedNode* node = *link;
					*link = node->next;
					delete node;
					return;
				}
				link = &(*link)->next;
			}
		}

		int* findSharedDepth() const
		{
			for (SharedNode* node = head(); node != nullptr; node = node->next)
			{
				if (node->id == id_)
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
			SharedNode* node = new SharedNode{id_, 0, head()};
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
		std::uint64_t id_;
		int exclusive_count_ = 0;
		int under_exclusive_ = 0;
	};

} // namespace rsm
