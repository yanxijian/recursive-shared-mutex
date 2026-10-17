cmake_minimum_required(VERSION 3.16)

# A consumer-only project: it is deliberately NOT part of the repository build.
# CI installs the package into a prefix and configures this directory against
# it, which is the path a downstream project actually takes: find_package()
# plus the imported target, with no access to the source tree.
project(rsm_consumer LANGUAGES CXX)

# SameMajorVersion, so asking for 0.4 accepts 0.4.x and rejects 0.5/1.0.
find_package(recursive_shared_mutex 0.4 REQUIRED CONFIG)

add_executable(rsm_consumer main.cpp)
target_link_libraries(rsm_consumer PRIVATE rsm::recursive_shared_mutex)
