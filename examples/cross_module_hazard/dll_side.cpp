// DLL side. Contains its own copy of rsm::RecursiveSharedMutex's inline code,
// hence its own module-local thread_local slot table.
#define RSM_HAZARD_BUILD_DLL 1
#include <system_error>

#include "iface.h"

namespace
{
	template <class F> int guard(F&& f)
	{
		try
		{
			f();
			return 0;
		}
		catch (const std::system_error& e)
		{
			return e.code().value();
		}
	}
} // namespace

extern "C" RSM_HAZARD_API int dll_lock_shared(Mutex* m)
{
	return guard([&] { m->lock_shared(); });
}

extern "C" RSM_HAZARD_API int dll_unlock_shared(Mutex* m)
{
	return guard([&] { m->unlock_shared(); });
}

extern "C" RSM_HAZARD_API int dll_try_lock(Mutex* m)
{
	int result = 0;
	const int err = guard([&] { result = m->try_lock() ? 1 : 0; });
	return err != 0 ? -err : result;
}
