ulw_add_library(curl_multi
    src/exchange.cpp
    src/fetcher.cpp
    src/http.cpp
    src/multi.cpp)
target_include_directories(curl_multi PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(curl_multi PUBLIC ulw::net_lib PRIVATE CURL::libcurl)
