ulw_add_library(net_lib
    src/send_queue.cpp
    src/timing_wheel.cpp)
target_include_directories(net_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(net_lib PUBLIC ulw::core_domain ulw::os)
