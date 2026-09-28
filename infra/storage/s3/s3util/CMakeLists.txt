ulw_add_library(s3util
    src/profile.cpp
    src/url.cpp)
target_include_directories(s3util PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(s3util PUBLIC ulw::core_domain)
