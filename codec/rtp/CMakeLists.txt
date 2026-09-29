ulw_add_library(codec_rtp
    src/error.cpp
    src/rtcp.cpp
    src/rtp.cpp)
target_include_directories(codec_rtp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Only core's lifetime.hpp.
target_link_libraries(codec_rtp PUBLIC ulw::core_domain)

if(ULW_FUZZ)
    target_compile_options(codec_rtp PRIVATE -fsanitize=fuzzer-no-link)
endif()
