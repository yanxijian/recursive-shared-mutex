add_executable(bench_rsm bench_vs_recursive_mutex.cpp)
target_link_libraries(bench_rsm PRIVATE rsm::recursive_shared_mutex)
set_target_properties(bench_rsm PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# The benchmark is useless without optimisation; make sure a Debug configure does
# not silently produce meaningless numbers.
if(MSVC)
	target_compile_options(bench_rsm PRIVATE /O2)
else()
	target_compile_options(bench_rsm PRIVATE -O2)
endif()

if(UNIX)
	find_package(Threads REQUIRED)
	target_link_libraries(bench_rsm PRIVATE Threads::Threads)
endif()
