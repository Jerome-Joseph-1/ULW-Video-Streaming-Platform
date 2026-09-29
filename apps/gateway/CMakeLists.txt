ulw_add_library(gateway_lib
    src/config.cpp
    src/connection.cpp
    src/gateway.cpp
    src/health.cpp
    src/key_fetcher.cpp
    src/playback.cpp
    src/view_recorder.cpp)
target_include_directories(gateway_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(gateway_lib PUBLIC
    ulw::core_domain ulw::net_lib ulw::http_lib ulw::auth ulw::curl_multi ulw::ops)

ulw_add_executable(gateway_server
    src/main.cpp)
target_link_libraries(gateway_server PRIVATE
    ulw::gateway_lib ulw::os ulw::storage_fs ulw::storage_s3 ulw::pg_store)
