#include <iostream>
#include <mutex>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <shared_mutex>

namespace
{

	rsm::RecursiveSharedMutex g_mtx;
	int g_value = 0;

	void nested_write(int v)
	{
		std::unique_lock<rsm::RecursiveSharedMutex> lock(g_mtx);
		g_value = v;
	}

	void write_with_nested_read(int v)
	{
		std::unique_lock<rsm::RecursiveSharedMutex> exclusive(g_mtx);
		{
			// Holding a write lock, nested shared_lock is allowed.
			std::shared_lock<rsm::RecursiveSharedMutex> shared(g_mtx);
			std::cout << "nested read sees " << g_value << '\n';
		}
		nested_write(v); // recursive exclusive re-entry
	}

	int read_value()
	{
		std::shared_lock<rsm::RecursiveSharedMutex> lock(g_mtx);
		return g_value;
	}

} // namespace

int main()
{
	write_with_nested_read(42);
	std::cout << "final value = " << read_value() << '\n';
	return 0;
}
