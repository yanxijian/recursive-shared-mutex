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

# Test-only failure-injection hook (fail_next_shared_acquire) inside the
# header; compiled out for normal consumers.
target_compile_definitions(rsm_tests PRIVATE RSM_ENABLE_TEST_HOOKS=1)

include(Catch)
catch_discover_tests(rsm_tests)
