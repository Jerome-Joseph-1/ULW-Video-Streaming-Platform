ulw_add_library(codec_ws
    src/decoder.cpp
    src/encoder.cpp
    src/handshake.cpp
    src/utf8.cpp)
target_include_directories(codec_ws PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(codec_ws PUBLIC ulw::core_domain ulw::http_lib PRIVATE OpenSSL::Crypto)

if(ULW_FUZZ)
    target_compile_options(codec_ws PRIVATE -fsanitize=fuzzer-no-link)
endif()
