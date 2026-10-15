ulw_add_library(notify_memory src/memory_connectivity.cpp)
target_include_directories(notify_memory
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(notify_memory PUBLIC ulw::core_domain ulw::net_lib)
