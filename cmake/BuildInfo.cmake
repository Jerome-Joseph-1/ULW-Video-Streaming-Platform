# The SHA is re-read on every build, not at configure time, so a binary never reports a
# commit it was not built from. The generated header is only rewritten when it changes.
set(ULW_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated)
add_custom_target(ulw_build_info
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DOUTPUT=${ULW_GENERATED_DIR}/ulw/build_info.inc
        -DVERSION=${PROJECT_VERSION}
        -P ${PROJECT_SOURCE_DIR}/cmake/WriteBuildInfo.cmake
    BYPRODUCTS ${ULW_GENERATED_DIR}/ulw/build_info.inc
    COMMENT "Stamping build info"
    VERBATIM)
