pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.5)

ulw_add_library(net_lib
    src/epoll_reactor.cpp
    src/ip_address.cpp
    src/job_queue.cpp
    src/offload_pool.cpp
    src/reactor_factory.cpp
    src/send_queue.cpp
    src/signals.cpp
    src/socket.cpp
    src/timing_wheel.cpp
    src/transport_plain.cpp
    src/transport_tls.cpp
    src/uring_reactor.cpp)
target_include_directories(net_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(net_lib
    PUBLIC ulw::core_domain ulw::os
    PRIVATE PkgConfig::LIBURING OpenSSL::SSL Threads::Threads)
