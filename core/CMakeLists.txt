ulw_add_library(core_domain
    src/version.cpp)
target_include_directories(core_domain
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${ULW_GENERATED_DIR})
add_dependencies(core_domain ulw_build_info)
