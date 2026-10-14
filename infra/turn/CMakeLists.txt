ulw_add_library(turn
    src/cloudflare.cpp
    src/config.cpp
    src/hmac.cpp
    src/relayed_sfu.cpp)
target_include_directories(turn PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(turn PUBLIC ulw::core_domain ulw::curl_multi PRIVATE OpenSSL::Crypto)
