ulw_add_library(chat_lib
    src/chat.cpp
    src/chat_service.cpp
    src/config.cpp
    src/envelope.cpp
    src/key_fetcher.cpp
    src/presence.cpp
    src/presence_room.cpp
    src/session.cpp
    src/token_bucket.cpp)
target_include_directories(chat_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(chat_lib PUBLIC
    ulw::core_domain ulw::net_lib ulw::http_lib ulw::codec_ws ulw::rt_lib ulw::auth
    ulw::curl_multi)
target_link_libraries(chat_lib PRIVATE OpenSSL::Crypto)

ulw_add_executable(chat_server
    src/main.cpp)
target_link_libraries(chat_server PRIVATE ulw::chat_lib ulw::os ulw::pg_rooms)
