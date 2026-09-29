ulw_add_library(reaper_lib
    src/config.cpp
    src/reaper.cpp)
target_include_directories(reaper_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(reaper_lib PUBLIC ulw::core_domain PRIVATE ulw::ops)

ulw_add_executable(ulw_reaper src/main.cpp)
target_link_libraries(ulw_reaper PRIVATE
    ulw::reaper_lib ulw::os ulw::ops ulw::net_lib ulw::pg_store ulw::storage_fs ulw::storage_s3)
