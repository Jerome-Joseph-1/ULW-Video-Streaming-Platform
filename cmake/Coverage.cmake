# Source-based coverage (clang only): the counters go on every target that links ulw_sanitize,
# which is every first-party target and also llhttp (http/CMakeLists.txt links it for the
# sanitizers); googletest and srt stay uninstrumented. llhttp's sources are under the build tree,
# which tools/coverage.sh leaves out of the report, so it is counted but never reported.
# tools/coverage.sh merges the profiles and writes the reports.
#
# A process that ends in _exit or _Exit (the ffmpeg sandbox helper, on every path; a forked
# child that does not exec) never runs the runtime's atexit writer and leaves no profile.
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
