#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <system_error>
#include <thread>

// C++17 header-only recursive shared_mutex.
// Exclusive: atomic owner id + owner-only counters on the instance.
// Shared recursion: thread_local intrusive list keyed by the mutex address.
//   Under the documented "never destroy while locked" contract, address reuse
//   can only inherit depth==0 nodes, and depth==0 nodes never fake
//   re-entrancy (lock_shared re-acquires the underlying lock).
// Cross-module note: the TLS list lives in every module that includes this
//   header. A mutex whose lock_shared/unlock_shared sequence crosses a
//   DLL/EXE boundary must stay within one module's TLS domain; if that cannot
//   be guaranteed, host the TLS state in a single exported module instead.

namespace rsm
{

	class RecursiveSharedMutex
	{
	public:
#ifdef RSM_ENABLE_TEST_HOOKS
		// Test instrumentation only: live TLS node count and cumulative TLS
		// list scan steps. Compiled out for normal consumers.
		inline static std::atomic<std::size_t> live_shared_nodes{0};
		inline static std::atomic<std::uint64_t> shared_scan_steps{0};
#endif

		RecursiveSharedMutex() = default;

		~RecursiveSharedMutex()
		{
			eraseSharedNode(this);
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

			// Allocate the TLS node before taking the underlying lock: if the
			// allocation throws, no shared slot has been acquired yet, so
			// bad_alloc can never strand a shared slot.
			SharedNode* fresh = (depth == nullptr) ? allocNode(this) : nullptr;
			try
			{
				mtx_.lock_shared();
			}
			catch (...)
			{
				freeNode(fresh);
				throw;
			}
			if (fresh != nullptr)
			{
				fresh->next = nodeList().head;
				nodeList().head = fresh;
				fresh->depth = 1;
			}
			else
			{
				*depth = 1;
			}
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

			SharedNode* fresh = (depth == nullptr) ? allocNode(this) : nullptr;
			bool acquired = false;
			try
			{
				acquired = mtx_.try_lock_shared();
			}
			catch (...)
			{
				freeNode(fresh);
				throw;
			}
			if (!acquired)
			{
				freeNode(fresh);
				return false;
			}
			if (fresh != nullptr)
			{
				fresh->next = nodeList().head;
				nodeList().head = fresh;
				fresh->depth = 1;
			}
			else
			{
				*depth = 1;
			}
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

		// thread_local intrusive list with an owning destructor so nodes are
		// released when a thread exits (previously leaked until process end).
		struct SharedNodeList
		{
			SharedNode* head = nullptr;

			~SharedNodeList()
			{
				while (head != nullptr)
				{
					SharedNode* node = head;
					head = node->next;
					freeNode(node);
				}
			}
		};

		static SharedNodeList& nodeList()
		{
			thread_local SharedNodeList list;
			return list;
		}

		static SharedNode* allocNode(const RecursiveSharedMutex* key)
		{
			SharedNode* node = new SharedNode{key, 0, nullptr};
#ifdef RSM_ENABLE_TEST_HOOKS
			live_shared_nodes.fetch_add(1, std::memory_order_relaxed);
#endif
			return node;
		}

		static void freeNode(SharedNode* node)
		{
			if (node == nullptr)
			{
				return;
			}
#ifdef RSM_ENABLE_TEST_HOOKS
			live_shared_nodes.fetch_sub(1, std::memory_order_relaxed);
#endif
			delete node;
		}

		static void eraseSharedNode(const RecursiveSharedMutex* key)
		{
			SharedNode** link = &nodeList().head;
			while (*link != nullptr)
			{
				if ((*link)->key == key)
				{
					SharedNode* node = *link;
					*link = node->next;
					freeNode(node);
					return;
				}
				link = &(*link)->next;
			}
		}

		int* findSharedDepth() const
		{
			SharedNode** link = &nodeList().head;
			SharedNode* prev = nullptr;
			for (SharedNode* node = *link; node != nullptr; node = node->next)
			{
#ifdef RSM_ENABLE_TEST_HOOKS
				shared_scan_steps.fetch_add(1, std::memory_order_relaxed);
#endif
				if (node->key == this)
				{
					if (prev != nullptr)
					{
						// Move-to-front so a hot mutex costs O(1) per lookup.
						prev->next = node->next;
						node->next = *link;
						*link = node;
					}
					return &node->depth;
				}
				prev = node;
			}
			return nullptr;
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
