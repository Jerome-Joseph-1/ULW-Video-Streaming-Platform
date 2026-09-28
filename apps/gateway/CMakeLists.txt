ulw_add_executable(gateway_server
    src/main.cpp)
target_link_libraries(gateway_server PRIVATE ulw::core_domain)
