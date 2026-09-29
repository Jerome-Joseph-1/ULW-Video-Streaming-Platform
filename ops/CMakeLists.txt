ulw_add_library(ops
    src/async_log.cpp
    src/log.cpp
    src/metrics.cpp
    src/process.cpp)
target_include_directories(ops PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ops PUBLIC ulw::core_domain ulw::os Threads::Threads)
