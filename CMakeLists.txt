cmake_minimum_required(VERSION 3.16)

project(recursive_shared_mutex
	VERSION 0.4.1
	DESCRIPTION "C++17 header-only recursive shared_mutex"
	HOMEPAGE_URL "https://github.com/yanxijian/recursive-shared-mutex"
	LANGUAGES CXX
)

# Keep VERSION in sync with RSM_VERSION_* in the header: the header is what
# FetchContent consumers see, this is what find_package() sees, and they used to
# disagree (0.1.0 here while v0.2.x was released).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(RSM_IS_TOP_LEVEL ON)
else()
	set(RSM_IS_TOP_LEVEL OFF)
endif()

# Default OFF when consumed via add_subdirectory()/FetchContent: a consumer must
# not be forced to fetch Catch2 and build two test binaries just to use a header.
option(RSM_BUILD_TESTS "Build unit tests" ${RSM_IS_TOP_LEVEL})
option(RSM_BUILD_EXAMPLES "Build examples" ${RSM_IS_TOP_LEVEL})

# Optional program-wide slot cap. The header defaults to 256; setting this
# cache variable makes the INTERFACE target propagate the same value to every
# target that links rsm::recursive_shared_mutex. Do not also set the macro per
# target: RSM_SHARED_SLOT_COUNT sizes the slot table in the class definition,
# so differing values across TUs are an ODR violation (IFNDR).
set(RSM_SHARED_SLOT_COUNT "" CACHE STRING
	"Override the per-thread shared-recursion slot cap (program-wide; positive integer)")

add_library(recursive_shared_mutex INTERFACE)
add_library(rsm::recursive_shared_mutex ALIAS recursive_shared_mutex)

target_include_directories(recursive_shared_mutex
	INTERFACE
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)

target_compile_features(recursive_shared_mutex INTERFACE cxx_std_17)

if(NOT RSM_SHARED_SLOT_COUNT STREQUAL "")
	if(NOT RSM_SHARED_SLOT_COUNT MATCHES "^[1-9][0-9]*$")
		message(FATAL_ERROR
			"RSM_SHARED_SLOT_COUNT must be a positive integer (got '${RSM_SHARED_SLOT_COUNT}')")
	endif()
	target_compile_definitions(recursive_shared_mutex
		INTERFACE RSM_SHARED_SLOT_COUNT=${RSM_SHARED_SLOT_COUNT})
endif()

# The header reports misuse with std::system_error, so it needs C++ exceptions.
# cl.exe enables them by default but clang-cl does not (/EHsc), and some MSVC
# project templates disable them. Ask for them here so consumers - and this
# repo's own clang-cl CI cell - do not have to know that.
if(MSVC)
	target_compile_options(recursive_shared_mutex INTERFACE /EHsc)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS recursive_shared_mutex
	EXPORT recursive_shared_mutexTargets
)

install(DIRECTORY include/
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT recursive_shared_mutexTargets
	FILE recursive_shared_mutexTargets.cmake
	NAMESPACE rsm::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/recursive_shared_mutex
)

configure_package_config_file(
	cmake/recursive_shared_mutexConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/recursive_shared_mutexConfig.cmake
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/recursive_shared_mutex
)

write_basic_package_version_file(
	${CMAKE_CURRENT_BINARY_DIR}/recursive_shared_mutexConfigVersion.cmake
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)

install(FILES
	${CMAKE_CURRENT_BINARY_DIR}/recursive_shared_mutexConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/recursive_shared_mutexConfigVersion.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/recursive_shared_mutex
)

if(RSM_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

if(RSM_BUILD_EXAMPLES)
	add_subdirectory(examples)
endif()

# Benchmark driver. Off by default; CI and benchmarks/run_bench.ps1 build it
# through this target so nothing has to hard-code a compiler path.
option(RSM_BUILD_BENCHMARKS "Build the benchmark driver (bench_rsm)" OFF)
if(RSM_BUILD_BENCHMARKS)
	add_subdirectory(benchmarks)
endif()
