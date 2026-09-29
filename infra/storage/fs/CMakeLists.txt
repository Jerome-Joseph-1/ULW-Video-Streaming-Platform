ulw_add_library(storage_fs src/fs_store.cpp)
target_include_directories(storage_fs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(storage_fs PUBLIC ulw::core_domain ulw::net_lib)

# Separate from storage_fs so that a process without a reactor can link it alone.
ulw_add_library(storage_fs_transfer src/fs_transfer.cpp)
target_include_directories(storage_fs_transfer
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(storage_fs_transfer PUBLIC ulw::core_domain PRIVATE ulw::os)
