ulw_add_library(http_lib
    src/origin.cpp
    src/request.cpp
    src/request_parser.cpp
    src/response.cpp
    src/router.cpp)
target_include_directories(http_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(http_lib PRIVATE llhttp_static)
# llhttp comes from its own project; it gets the same sanitizers, so a wrong length or a
# dangling buffer handed to it is caught inside it instead of surfacing later in ours.
target_link_libraries(llhttp_static PRIVATE $<BUILD_INTERFACE:ulw_sanitize>)

if(ULW_FUZZ)
    # Coverage feedback has to come from the parser and llhttp, not just from the harness.
    target_compile_options(http_lib PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_options(llhttp_static PRIVATE -fsanitize=fuzzer-no-link)
endif()
