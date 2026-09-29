ulw_add_library(sfu_livekit
    src/access_token.cpp
    src/livekit_sfu.cpp
    src/room_service.cpp)
target_include_directories(sfu_livekit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(sfu_livekit PUBLIC ulw::core_domain ulw::curl_multi PRIVATE ulw::auth
    OpenSSL::Crypto)
