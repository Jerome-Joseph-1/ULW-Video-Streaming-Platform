ulw_add_library(codec_sdp
    src/attributes.cpp
    src/error.cpp
    src/parser.cpp
    src/serializer.cpp
    src/validate.cpp)
target_include_directories(codec_sdp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# core's header-only number parsing, and lifetime.hpp in the public header.
target_link_libraries(codec_sdp PUBLIC ulw::core_domain)

if(ULW_FUZZ)
    target_compile_options(codec_sdp PRIVATE -fsanitize=fuzzer-no-link)
endif()
