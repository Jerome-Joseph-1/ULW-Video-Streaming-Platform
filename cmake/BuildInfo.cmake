# The SHA is re-read on every build, not at configure time, so a binary never reports a
# commit it was not built from. The generated header is only rewritten when it changes.
set(ULW_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated)
# A container build copies the tree without .git; it hands the commit in instead.
set(ULW_GIT_SHA "" CACHE STRING "Commit to stamp when the source tree carries no git metadata")
add_custom_target(ulw_build_info
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DOUTPUT=${ULW_GENERATED_DIR}/ulw/build_info.inc
        -DVERSION=${PROJECT_VERSION}
        -DGIT_SHA=${ULW_GIT_SHA}
        -P ${PROJECT_SOURCE_DIR}/cmake/WriteBuildInfo.cmake
    BYPRODUCTS ${ULW_GENERATED_DIR}/ulw/build_info.inc
    COMMENT "Stamping build info"
    VERBATIM)
