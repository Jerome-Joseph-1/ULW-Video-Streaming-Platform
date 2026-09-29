find_package(Git QUIET)
set(sha "unknown")
if(GIT_SHA)
    set(sha ${GIT_SHA})
elseif(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} rev-parse --short=12 HEAD
        OUTPUT_VARIABLE git_out OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_rc ERROR_QUIET)
    if(git_rc EQUAL 0 AND git_out)
        set(sha ${git_out})
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} diff --quiet HEAD --
            RESULT_VARIABLE dirty_rc ERROR_QUIET)
        if(NOT dirty_rc EQUAL 0)
            string(APPEND sha "-dirty")
        endif()
    endif()
endif()
set(content "#define ULW_VERSION \"${VERSION}\"\n#define ULW_GIT_SHA \"${sha}\"\n")
if(EXISTS ${OUTPUT})
    file(READ ${OUTPUT} previous)
endif()
if(NOT "${previous}" STREQUAL "${content}")
    file(WRITE ${OUTPUT} "${content}")
endif()
