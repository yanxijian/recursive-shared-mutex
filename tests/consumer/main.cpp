// Built by the "install + find_package" CI job. If the exported target, its
// include directory or the package version file is wrong, this does not compile
// or does not link - which is the point: the repository's own builds never take
// this path.
#include <cstdio>
#include <mutex>
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <shared_mutex>

int main()
{
	rsm::RecursiveSharedMutex mtx;
	{
		std::unique_lock<rsm::RecursiveSharedMutex> lock(mtx);
		if (!mtx.is_held_exclusive())
		{
			std::puts("is_held_exclusive() missed the owner");
			return 1;
		}
	}
	{
		std::shared_lock<rsm::RecursiveSharedMutex> lock(mtx);
		if (mtx.is_held_exclusive())
		{
			std::puts("is_held_exclusive() reported a shared holder");
			return 2;
		}
	}
	if (mtx.try_lock())
	{
		mtx.unlock();
	}
	else
	{
		std::puts("the mutex is not usable after the scopes above");
		return 3;
	}
	std::printf("rsm %d.%d.%d ok\n", RSM_VERSION_MAJOR, RSM_VERSION_MINOR, RSM_VERSION_PATCH);
	return 0;
}
