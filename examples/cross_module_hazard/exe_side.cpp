// EXE side of the cross-module hazard demo.
//
// Claim under test (docs/migration.md, "Cross-module (DLL/EXE) use"):
//   a mutex whose lock_shared/unlock_shared sequence crosses a module boundary
//   must stay inside one module's TLS domain, otherwise the accounting splits.
//
// What this shows on Windows with a real DLL:
//   step 1  the DLL cannot see the EXE's slot            -> separate TLS domains
//   step 2  the same thread acquires the backend TWICE   -> the second
//           mtx_.lock_shared() is UB per [thread.sharedmutex]
//   step 3  the EXE can no longer release the DLL's level -> a writer stays
//           blocked until the DLL releases
// On Windows (SRWLOCK) none of this throws, which is exactly why it is
// dangerous: the extra backend hold only shows up as a writer that never gets
// in. On a writer-preferring rwlock the same sequence can deadlock the thread.

#include <cstdio>
#include <string>
#include <system_error>
#include <thread>

#include "iface.h"

namespace
{
	// Another thread probing the mutex; true = the exclusive lock was free.
	bool writerCanEnter(Mutex& m)
	{
		bool got = false;
		std::thread w([&] {
			got = m.try_lock();
			if (got)
			{
				m.unlock(); // must be released by the acquiring thread
			}
		});
		w.join();
		return got;
	}

	int exeUnlockShared(Mutex& m)
	{
		try
		{
			m.unlock_shared();
			return 0;
		}
		catch (const std::system_error& e)
		{
			return e.code().value();
		}
	}

	std::string code(int err)
	{
		return err == 0 ? "ok" : ("error " + std::to_string(err));
	}
} // namespace

int main()
{
	Mutex m;
	std::printf("cross-module TLS domain demo (mutex object lives in the EXE)\n");

	// ---------------- step 1 ----------------
	std::printf("\nstep 1: EXE holds shared, DLL tries to release it\n");
	m.lock_shared();
	{
		const int err = dll_unlock_shared(&m);
		std::printf("  dll_unlock_shared      -> %s%s\n", code(err).c_str(),
		            err == 0 ? "  (single shared table - NOT expected)" : "  <- separate TLS domain");
	}
	std::printf("  writer can enter?       -> %s (expected NO, the EXE still holds)\n", writerCanEnter(m) ? "YES" : "no");
	m.unlock_shared();

	// ---------------- step 2 ----------------
	std::printf("\nstep 2: same thread takes shared in the EXE, then again inside the DLL\n");
	m.lock_shared();
	const int dllErr = dll_lock_shared(&m);
	std::printf("  dll_lock_shared        -> %s  (returns silently: it called\n", code(dllErr).c_str());
	std::printf("                            std::shared_mutex::lock_shared() a second time)\n");
	std::printf("  writer can enter?       -> %s (expected NO)\n", writerCanEnter(m) ? "YES" : "no");

	// ---------------- step 3 ----------------
	std::printf("\nstep 3: release the EXE level, then look again\n");
	const int first = exeUnlockShared(m);
	std::printf("  EXE unlock_shared #1   -> %s\n", code(first).c_str());
	std::printf("  writer can enter?       -> %s (expected NO: the DLL's hold is still there)\n", writerCanEnter(m) ? "YES" : "no");

	const int second = exeUnlockShared(m);
	std::printf("  EXE unlock_shared #2   -> %s%s\n", code(second).c_str(),
	            second == 0 ? "  (single shared table - NOT expected)" : "  <- EXE has no slot for that level");
	const int dllRelease = dll_unlock_shared(&m);
	std::printf("  dll_unlock_shared      -> %s\n", code(dllRelease).c_str());
	std::printf("  writer can enter?       -> %s (expected YES)\n", writerCanEnter(m) ? "YES" : "no");

	std::printf("\nconclusion\n");
	std::printf("  one thread called std::shared_mutex::lock_shared() twice without an\n");
	std::printf("  intervening unlock: [thread.sharedmutex] undefined behaviour, silently\n");
	std::printf("  accepted by SRWLOCK. The only symptom is that module A cannot release\n");
	std::printf("  module B's level. Keep one mutex's shared sequence inside one module,\n");
	std::printf("  or host the TLS table in a single exported module.\n");
	return 0;
}
