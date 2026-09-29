ulw_add_library(srt_ingest src/ingest.cpp)
target_include_directories(srt_ingest PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# srt.h is not exported by libsrt's own targets, and the dependency block that fetched it is
# scoped, so its directories are asked for by name.
FetchContent_GetProperties(srt SOURCE_DIR ulw_srt_source BINARY_DIR ulw_srt_binary)
target_include_directories(srt_ingest SYSTEM PRIVATE ${ulw_srt_source}/srtcore ${ulw_srt_binary})
target_link_libraries(srt_ingest PUBLIC ulw::core_domain PRIVATE srt_static)
