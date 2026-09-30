ulw_add_library(messages_memory src/memory_message_store.cpp)
target_include_directories(messages_memory PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(messages_memory PUBLIC ulw::core_domain ulw::net_lib ulw::rt_lib)
