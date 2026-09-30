add_library(ulw_warnings INTERFACE)

set(ulw_cxx_warnings
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wconversion -Wsign-conversion -Wdouble-promotion -Wformat=2
    -Wimplicit-fallthrough -Wnull-dereference -Wmisleading-indentation)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND ulw_cxx_warnings
        -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast)
endif()

if(ULW_WERROR)
    list(APPEND ulw_cxx_warnings -Werror)
endif()

target_compile_options(ulw_warnings INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:${ulw_cxx_warnings}>")

# Hardening, apart from the warnings so the vendored C and C++ that is linked into the same
# binaries (llhttp, srt) gets it too without taking on this project's -Werror (Dependencies.cmake).
# Clang, unlike Ubuntu's GCC, enables none of it by default: one object without -fcf-protection
# drops the binary's IBT and SHSTK markings (tools/check-hardening.sh, docs/adr/0072).
# A list, not a target: llhttp and srt install(EXPORT) their targets, which may not name one of
# ours.
set(ulw_hardening_flags
    -fstack-protector-strong -fstack-clash-protection -fcf-protection
    # _FORTIFY_SOURCE needs optimisation to do anything and warns at -O0.
    "$<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>")
target_compile_options(ulw_warnings INTERFACE ${ulw_hardening_flags})
target_link_options(ulw_warnings INTERFACE
    -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
