ulw_add_library(os
    src/limits.cpp
    src/privileges.cpp
    src/system_clock.cpp
    src/system_random.cpp
    src/unique_fd.cpp)
target_include_directories(os PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(os PUBLIC ulw::core_domain)
