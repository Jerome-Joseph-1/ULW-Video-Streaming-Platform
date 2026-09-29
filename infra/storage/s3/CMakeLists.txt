ulw_add_library(storage_s3
    src/control.cpp
    src/endpoint.cpp
    src/failure.cpp
    src/part_session.cpp
    src/s3_store.cpp
    src/s3_transfer.cpp)
target_include_directories(storage_s3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(storage_s3 PUBLIC ulw::core_domain ulw::net_lib ulw::curl_multi ulw::s3util ulw::os)
