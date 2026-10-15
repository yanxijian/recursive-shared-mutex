add_executable(basic_usage basic_usage.cpp)
target_link_libraries(basic_usage PRIVATE rsm::recursive_shared_mutex)
