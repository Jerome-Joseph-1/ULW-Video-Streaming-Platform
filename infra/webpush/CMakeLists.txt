ulw_add_library(webpush
    src/curl_transport.cpp
    src/ece.cpp
    src/endpoint.cpp
    src/p256.cpp
    src/sender.cpp
    src/vapid.cpp)
target_include_directories(webpush PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(webpush
    PUBLIC ulw::core_domain ulw::net_lib ulw::curl_multi
    PRIVATE ulw::auth OpenSSL::Crypto)
