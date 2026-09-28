ulw_add_library(catalog_memory src/memory_catalog.cpp)
target_include_directories(catalog_memory PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(catalog_memory PUBLIC ulw::core_domain ulw::net_lib)
