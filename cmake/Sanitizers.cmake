# Always defined so targets can link it unconditionally; empty unless ULW_SANITIZE is set.
add_library(ulw_sanitize INTERFACE)

if(ULW_SANITIZE)
    if(ULW_SANITIZE MATCHES "address" AND ULW_SANITIZE MATCHES "thread")
        message(FATAL_ERROR "ASan and TSan cannot share a build (ULW_SANITIZE=${ULW_SANITIZE}).")
    endif()
    set(ulw_san_flags
        -fsanitize=${ULW_SANITIZE} -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)
    if(ULW_SANITIZE MATCHES "undefined")
        # UBSan otherwise reports and carries on; a finding must fail the test.
        list(APPEND ulw_san_flags -fno-sanitize-recover=undefined)
    endif()
    target_compile_options(ulw_sanitize INTERFACE ${ulw_san_flags})
    target_link_options(ulw_sanitize INTERFACE -fsanitize=${ULW_SANITIZE})
endif()
