cmake_minimum_required(VERSION 3.16)

project(recursive_shared_mutex
	VERSION 0.2.1
	DESCRIPTION "C++17 header-only recursive shared_mutex"
	HOMEPAGE_URL "https://github.com/yanxijian/recursive-shared-mutex"
	LANGUAGES CXX
)

# Single source of truth for the version: bump it here and in CHANGELOG.md
# together. (Historically this stayed at 0.1.0 while v0.2.x was released, which
# made `find_package(recursive_shared_mutex 0.2 REQUIRED)` fail.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(RSM_IS_TOP_LEVEL ON)
else()
	set(RSM_IS_TOP_LEVEL OFF)
endif()

# Default OFF when consumed via add_subdirectory()/FetchContent: a consumer must
# not be forced to fetch Catch2 and build two test binaries just to use a header.
option(RSM_BUILD_TESTS "Build unit tests" ${RSM_IS_TOP_LEVEL})
option(RSM_BUILD_EXAMPLES "Build examples" ${RSM_IS_TOP_LEVEL})

add_library(recursive_shared_mutex INTERFACE)
add_library(rsm::recursive_shared_mutex ALIAS recursive_shared_mutex)

target_include_directories(recursive_shared_mutex
	INTERFACE
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)

target_compile_features(recursive_shared_mutex INTERFACE cxx_std_17)

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
