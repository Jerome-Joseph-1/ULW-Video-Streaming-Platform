ulw_add_library(core_domain
    src/content_type.cpp
    src/domain_error.cpp
    src/hls.cpp
    src/port_errors.cpp
    src/ids.cpp
    src/json.cpp
    src/key_package.cpp
    src/ladder.cpp
    src/storage_error.cpp
    src/storage_key.cpp
    src/upload.cpp
    src/uuid.cpp
    src/version.cpp
    src/video.cpp)
target_include_directories(core_domain
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${ULW_GENERATED_DIR})
add_dependencies(core_domain ulw_build_info)
