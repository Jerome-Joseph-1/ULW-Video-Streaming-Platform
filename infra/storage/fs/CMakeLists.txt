ulw_add_library(storage_fs src/fs_store.cpp)
target_include_directories(storage_fs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(storage_fs PUBLIC ulw::core_domain ulw::net_lib)
