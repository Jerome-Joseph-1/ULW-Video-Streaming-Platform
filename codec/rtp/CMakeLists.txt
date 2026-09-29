ulw_add_library(codec_rtp
    src/error.cpp
    src/rtcp.cpp
    src/rtp.cpp)
target_include_directories(codec_rtp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(ULW_FUZZ)
    target_compile_options(codec_rtp PRIVATE -fsanitize=fuzzer-no-link)
endif()
