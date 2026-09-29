ulw_add_library(live_packager_lib
    src/config.cpp
    src/live_window.cpp
    src/media_playlist.cpp
    src/publisher.cpp
    src/segment_tracker.cpp
    src/stream_id.cpp
    src/stream_runner.cpp
    src/watch.cpp)
target_include_directories(live_packager_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(live_packager_lib PUBLIC
    ulw::core_domain ulw::ffmpeg_adapter ulw::srt_ingest Threads::Threads PRIVATE ulw::os)

# A process of its own, not linked into the gateway (ADR-0043). It reuses the worker's
# sandboxed ffmpeg and blocking object transfer, and none of the reactor.
ulw_add_executable(live_packager src/main.cpp)
target_link_libraries(live_packager PRIVATE
    ulw::live_packager_lib ulw::os ulw::storage_fs_transfer ulw::storage_s3 ulw::s3util)
# Like the worker, it looks for the sandbox helper beside its own executable.
add_custom_command(TARGET live_packager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ulw_sandbox>
            $<TARGET_FILE_DIR:live_packager>/ulw_sandbox)
add_dependencies(live_packager ulw_sandbox)
