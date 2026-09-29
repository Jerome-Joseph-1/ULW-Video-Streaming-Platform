ulw_add_library(rt_lib
    src/node_auth.cpp
    src/registry.cpp
    src/room_router.cpp
    src/wire.cpp)
target_include_directories(rt_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(rt_lib
    PUBLIC ulw::core_domain ulw::net_lib ulw::os
    PRIVATE OpenSSL::Crypto)
