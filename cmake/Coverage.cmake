# Source-based coverage (clang only): the counters go on every first-party target through
# ulw_sanitize, which each one links, so third-party code (llhttp, googletest, srt) stays
# uninstrumented. tools/coverage.sh merges the profiles and writes the reports.
#
# The profile path is built into each binary rather than left to LLVM_PROFILE_FILE, because the
# harnesses start the servers they test with an environment of their own. %8m merges each
# binary's profiles into at most eight files, however many processes a suite starts.
if(ULW_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ULW_COVERAGE requires clang (source-based coverage).")
    endif()
    set(ulw_profile ${PROJECT_BINARY_DIR}/coverage/profiles/ulw-%8m.profraw)
    target_compile_options(ulw_sanitize INTERFACE
        -fprofile-instr-generate=${ulw_profile} -fcoverage-mapping)
    target_link_options(ulw_sanitize INTERFACE -fprofile-instr-generate=${ulw_profile})
endif()
