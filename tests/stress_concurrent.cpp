#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <shared_mutex>
#include <thread>
#include <vector>

int main()
{
	rsm::RecursiveSharedMutex mtx;
	std::atomic<int> value{0};
	std::atomic<int> readers_ok{0};
	constexpr int kReaders = 8;
	constexpr int kIters = 200;

	std::thread writer([&]() {
		for (int i = 0; i < kIters; ++i)
		{
			std::unique_lock<rsm::RecursiveSharedMutex> lock(mtx);
			value.store(i, std::memory_order_relaxed);
			std::this_thread::sleep_for(std::chrono::microseconds(50));
			value.store(i, std::memory_order_relaxed);
		}
	});

	std::vector<std::thread> readers;
	readers.reserve(static_cast<std::size_t>(kReaders));
	for (int r = 0; r < kReaders; ++r)
	{
		readers.emplace_back([&]() {
			for (int i = 0; i < kIters; ++i)
			{
				std::shared_lock<rsm::RecursiveSharedMutex> lock(mtx);
				(void)value.load(std::memory_order_relaxed);
				readers_ok.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	writer.join();
	for (auto& t : readers)
	{
		t.join();
	}

	const int expected = kReaders * kIters;
	std::cout << "readers_ok=" << readers_ok << " expected=" << expected << '\n';
	return readers_ok == expected ? 0 : 1;
}
