# How the stream service starts a stream's packager (ADR-0091): a child process, or a Kubernetes
# Job through the API server.
ulw_add_library(packagers
    src/kubernetes_packagers.cpp
    src/process_packagers.cpp)
target_include_directories(packagers PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(packagers PUBLIC ulw::core_domain ulw::net_lib ulw::curl_multi ulw::os)
