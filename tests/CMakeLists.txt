include(FetchContent)

FetchContent_Declare(
	Catch2
	GIT_REPOSITORY https://github.com/catchorg/Catch2.git
	GIT_TAG v3.5.4
)

FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(rsm_tests
	test_recursive_shared_mutex.cpp
)

target_link_libraries(rsm_tests
	PRIVATE
		rsm::recursive_shared_mutex
		Catch2::Catch2WithMain
)

add_executable(rsm_metrics_tests
	test_instrumented_write_lock.cpp
)
target_link_libraries(rsm_metrics_tests
	PRIVATE
		rsm::recursive_shared_mutex
		Catch2::Catch2WithMain
)

# Test-only failure-injection hook (fail_next_shared_acquire) inside the
# header; compiled out for normal consumers.
target_compile_definitions(rsm_tests PRIVATE RSM_ENABLE_TEST_HOOKS=1)

include(Catch)
catch_discover_tests(rsm_tests)
catch_discover_tests(rsm_metrics_tests)

# --- framework-free regression tests -----------------------------------------
# These two do not use Catch2 on purpose: every CI cell (sanitizers, MinGW,
# alpine/musl, 32-bit) can build and run them with a one-line compiler call, and
# the backend contract test must run before anything else because it validates
# the *toolchain*, not the library.
add_executable(backend_contract ${CMAKE_CURRENT_LIST_DIR}/backend_contract.cpp)
add_executable(mutual_exclusion ${CMAKE_CURRENT_LIST_DIR}/mutual_exclusion.cpp)

foreach(tgt backend_contract mutual_exclusion)
	target_link_libraries(${tgt} PRIVATE rsm::recursive_shared_mutex)
	set_target_properties(${tgt} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endforeach()

if(UNIX)
	find_package(Threads REQUIRED)
	target_link_libraries(backend_contract PRIVATE Threads::Threads)
	target_link_libraries(mutual_exclusion PRIVATE Threads::Threads)
endif()

add_test(NAME backend_contract COMMAND backend_contract --rounds 15 --iters 3000)
add_test(NAME mutual_exclusion COMMAND mutual_exclusion --iters 20000)
set_tests_properties(backend_contract mutual_exclusion PROPERTIES TIMEOUT 180)
