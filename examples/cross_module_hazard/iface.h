// Shared interface between the "host" side and the "library" side of the
// cross-module hazard demo. Both sides include the recursive-shared-mutex
// header, so each module gets its own copy of the inline code and therefore its
// own thread_local slot table.
#pragma once

#include <recursive_shared_mutex/recursive_shared_mutex.hpp>

using Mutex = rsm::RecursiveSharedMutex;

#if defined(_WIN32)
#if defined(RSM_HAZARD_BUILD_DLL)
#define RSM_HAZARD_API __declspec(dllexport)
#else
#define RSM_HAZARD_API __declspec(dllimport)
#endif
#else
#define RSM_HAZARD_API __attribute__((visibility("default")))
#endif

extern "C"
{
	// Return 0 on success, otherwise the std::errc value of the thrown error.
	RSM_HAZARD_API int dll_lock_shared(Mutex* m);
	RSM_HAZARD_API int dll_unlock_shared(Mutex* m);
	RSM_HAZARD_API int dll_try_lock(Mutex* m);
}
