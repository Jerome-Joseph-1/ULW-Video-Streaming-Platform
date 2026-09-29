# The OpenMLS bridge: a Rust staticlib built by cargo, and the C++ wrapper over its C API.
# Cargo runs --locked --offline: Cargo.lock pins every crate by version and SHA-256, and the
# one step that touches the network is the mls_ffi_fetch target (cargo fetch --locked).
find_program(ULW_CARGO cargo REQUIRED)

set(ulw_mls_crate ${CMAKE_CURRENT_SOURCE_DIR}/mls_ffi_bridge)
set(ulw_mls_target_dir ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(ulw_mls_staticlib ${ulw_mls_target_dir}/release/libmls_ffi_bridge.a)
file(GLOB_RECURSE ulw_mls_rust_sources CONFIGURE_DEPENDS ${ulw_mls_crate}/src/*.rs)

# Always the release profile: debug-built RustCrypto is tens of times slower, and the Rust side
# is not instrumented under any preset anyway (ADR-0039).
add_custom_command(
    OUTPUT ${ulw_mls_staticlib}
    COMMAND ${ULW_CARGO} build --release --locked --offline
        --manifest-path ${ulw_mls_crate}/Cargo.toml --target-dir ${ulw_mls_target_dir}
    # rustup reads rust-toolchain.toml from the working directory.
    WORKING_DIRECTORY ${ulw_mls_crate}
    DEPENDS ${ulw_mls_rust_sources} ${ulw_mls_crate}/Cargo.toml ${ulw_mls_crate}/Cargo.lock
        ${ulw_mls_crate}/rust-toolchain.toml
    COMMENT "Building the OpenMLS bridge"
    USES_TERMINAL
    VERBATIM)
add_custom_target(mls_ffi_bridge_build DEPENDS ${ulw_mls_staticlib})

add_custom_target(mls_ffi_fetch
    COMMAND ${ULW_CARGO} fetch --locked --manifest-path ${ulw_mls_crate}/Cargo.toml
    WORKING_DIRECTORY ${ulw_mls_crate}
    COMMENT "Fetching the OpenMLS bridge's crates"
    USES_TERMINAL
    VERBATIM)

# What rustc --print native-static-libs lists for a std staticlib on Linux.
add_library(mls_ffi_bridge STATIC IMPORTED GLOBAL)
set_target_properties(mls_ffi_bridge PROPERTIES
    IMPORTED_LOCATION ${ulw_mls_staticlib}
    INTERFACE_INCLUDE_DIRECTORIES ${ulw_mls_crate}/include
    INTERFACE_LINK_LIBRARIES "gcc_s;util;rt;Threads::Threads;m;${CMAKE_DL_LIBS}")
add_dependencies(mls_ffi_bridge mls_ffi_bridge_build)

ulw_add_library(e2ee_mls src/mls.cpp)
target_include_directories(e2ee_mls PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(e2ee_mls PRIVATE mls_ffi_bridge)
