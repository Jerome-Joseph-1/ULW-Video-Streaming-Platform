ulw_add_library(worker_lib
    src/config.cpp
    src/heartbeat.cpp
    src/job_runner.cpp
    src/lease_keeper.cpp
    src/worker.cpp
    src/workspace.cpp)
target_include_directories(worker_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(worker_lib PUBLIC ulw::core_domain ulw::ops Threads::Threads PRIVATE ulw::os)

ulw_add_executable(transcode_worker src/main.cpp)
target_link_libraries(transcode_worker PRIVATE
    ulw::worker_lib ulw::os ulw::pg_store ulw::ffmpeg_adapter ulw::storage_fs_transfer
    ulw::storage_s3 ulw::s3util)
# The worker looks for the sandbox helper beside its own executable.
set_target_properties(ulw_sandbox PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(transcode_worker ulw_sandbox)
