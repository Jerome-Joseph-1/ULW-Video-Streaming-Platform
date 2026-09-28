ulw_add_library(storage_fake src/fake_store.cpp)
target_include_directories(storage_fake PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(storage_fake PUBLIC ulw::core_domain ulw::net_lib)
