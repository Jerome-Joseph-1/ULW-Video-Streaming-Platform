ulw_add_library(e2ee_directory_memory src/memory_directory.cpp)
target_include_directories(e2ee_directory_memory
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(e2ee_directory_memory PUBLIC ulw::core_domain ulw::net_lib)
