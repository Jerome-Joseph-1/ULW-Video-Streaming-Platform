ulw_add_library(gateway_lib
    src/connection.cpp
    src/gateway.cpp)
target_include_directories(gateway_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(gateway_lib PUBLIC ulw::core_domain ulw::net_lib ulw::http_lib)

ulw_add_executable(gateway_server
    src/main.cpp)
target_link_libraries(gateway_server PRIVATE ulw::core_domain)
