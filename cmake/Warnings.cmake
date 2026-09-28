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

# Hardening applies to C as well: llhttp is linked into the same binary.
target_compile_options(ulw_warnings INTERFACE
    -fstack-protector-strong -fstack-clash-protection -fcf-protection
    # _FORTIFY_SOURCE needs optimisation to do anything and warns at -O0.
    "$<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>")
target_link_options(ulw_warnings INTERFACE
    -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
