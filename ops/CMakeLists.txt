ulw_add_library(ops
    src/allocator.cpp
    src/async_log.cpp
    src/dev_only.cpp
    src/log.cpp
    src/metrics.cpp
    src/notify.cpp
    src/process.cpp
    src/root.cpp
    src/settings.cpp
    src/toml.cpp)
target_include_directories(ops PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ops PUBLIC ulw::core_domain ulw::os Threads::Threads)
