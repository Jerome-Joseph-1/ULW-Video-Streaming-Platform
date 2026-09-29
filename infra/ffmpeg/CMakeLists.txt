ulw_add_library(ffmpeg_adapter
    src/command.cpp
    src/exit_code.cpp
    src/live_command.cpp
    src/live_remux.cpp
    src/process.cpp
    src/progress.cpp
    src/transcoder.cpp)
target_include_directories(ffmpeg_adapter PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ffmpeg_adapter PUBLIC ulw::core_domain PRIVATE ulw::os)

# Every ffmpeg and ffprobe child is started through this; see src/sandbox_main.cpp. Never
# sanitized, so it links no instrumented library either: ASan's runtime needs fresh mappings
# after the helper has lowered RLIMIT_AS below the terabytes of shadow memory it reserved, and
# dies trying. It uses only header-only parts of core.
add_executable(ulw_sandbox src/sandbox_main.cpp)
target_compile_features(ulw_sandbox PRIVATE cxx_std_23)
target_include_directories(ulw_sandbox PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
target_link_libraries(ulw_sandbox PRIVATE ulw_warnings)
