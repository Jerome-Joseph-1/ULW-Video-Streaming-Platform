# The OpenMLS bridge: a Rust staticlib built by cargo, and the C++ wrapper over its C API.
# Cargo runs --locked --offline: Cargo.lock pins every crate by version and SHA-256, and the
# one step that touches the network is the mls_ffi_fetch target, which installs the pinned
# toolchain and fetches the crates. With RUSTUP_AUTO_INSTALL=0 a build that lacks the toolchain
# fails at once instead of downloading it.
find_program(ULW_CARGO cargo REQUIRED)
set(ULW_CARGO_OFFLINE ${CMAKE_COMMAND} -E env RUSTUP_AUTO_INSTALL=0 ${ULW_CARGO}
    CACHE INTERNAL "cargo, never installing a toolchain")

set(ulw_mls_crate ${CMAKE_CURRENT_SOURCE_DIR}/mls_ffi_bridge)
set(ulw_mls_target_dir ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(ulw_mls_staticlib ${ulw_mls_target_dir}/release/libmls_ffi_bridge.a)
file(GLOB_RECURSE ulw_mls_rust_sources CONFIGURE_DEPENDS ${ulw_mls_crate}/src/*.rs)

# Always the release profile: debug-built RustCrypto is tens of times slower, and the Rust side
# is not instrumented under any preset anyway (ADR-0039).
add_custom_command(
    OUTPUT ${ulw_mls_staticlib}
    COMMAND ${ULW_CARGO_OFFLINE} build --release --locked --offline
        --manifest-path ${ulw_mls_crate}/Cargo.toml --target-dir ${ulw_mls_target_dir}
    # rustup reads rust-toolchain.toml from the working directory.
    WORKING_DIRECTORY ${ulw_mls_crate}
    DEPENDS ${ulw_mls_rust_sources} ${ulw_mls_crate}/Cargo.toml ${ulw_mls_crate}/Cargo.lock
        ${ulw_mls_crate}/rust-toolchain.toml
    COMMENT "Building the OpenMLS bridge"
    USES_TERMINAL
    VERBATIM)
add_custom_target(mls_ffi_bridge_build DEPENDS ${ulw_mls_staticlib})

find_program(ULW_RUSTUP rustup)
add_custom_target(mls_ffi_fetch
    COMMAND ${ULW_RUSTUP} toolchain install --no-self-update
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
# For tests that run cargo against the same build: tests/CMakeLists.txt reads these.
set_target_properties(mls_ffi_bridge PROPERTIES
    ULW_CRATE_DIR ${ulw_mls_crate}
    ULW_CARGO_TARGET_DIR ${ulw_mls_target_dir})

# libFuzzer steers by coverage, and the release library above has none: the bridge's decoding
# is all Rust. The fuzz target links a second build carrying the same SanitizerCoverage
# instrumentation clang's -fsanitize=fuzzer adds, the flags cargo-fuzz passes, all on stable.
# --target keeps RUSTFLAGS off build scripts and proc macros, which run in the compiler.
if(ULW_FUZZ)
    set(ulw_mls_fuzz_triple ${CMAKE_SYSTEM_PROCESSOR}-unknown-linux-gnu)
    set(ulw_mls_fuzz_dir ${CMAKE_CURRENT_BINARY_DIR}/cargo-fuzz)
    set(ulw_mls_fuzz_lib ${ulw_mls_fuzz_dir}/${ulw_mls_fuzz_triple}/release/libmls_ffi_bridge.a)
    set(ulw_sancov_flags
        "-Cpasses=sancov-module -Cllvm-args=-sanitizer-coverage-level=4 \
-Cllvm-args=-sanitizer-coverage-inline-8bit-counters -Cllvm-args=-sanitizer-coverage-pc-table \
-Cllvm-args=-sanitizer-coverage-trace-compares")
    add_custom_command(
        OUTPUT ${ulw_mls_fuzz_lib}
        COMMAND ${CMAKE_COMMAND} -E env "RUSTFLAGS=${ulw_sancov_flags}" RUSTUP_AUTO_INSTALL=0
            ${ULW_CARGO} build --release --locked --offline --target ${ulw_mls_fuzz_triple}
            --manifest-path ${ulw_mls_crate}/Cargo.toml --target-dir ${ulw_mls_fuzz_dir}
        WORKING_DIRECTORY ${ulw_mls_crate}
        DEPENDS ${ulw_mls_rust_sources} ${ulw_mls_crate}/Cargo.toml ${ulw_mls_crate}/Cargo.lock
            ${ulw_mls_crate}/rust-toolchain.toml
        COMMENT "Building the OpenMLS bridge with coverage for libFuzzer"
        USES_TERMINAL
        VERBATIM)
    add_custom_target(mls_ffi_bridge_fuzz_build DEPENDS ${ulw_mls_fuzz_lib})
    add_library(mls_ffi_bridge_fuzz STATIC IMPORTED GLOBAL)
    set_target_properties(mls_ffi_bridge_fuzz PROPERTIES
        IMPORTED_LOCATION ${ulw_mls_fuzz_lib}
        INTERFACE_INCLUDE_DIRECTORIES ${ulw_mls_crate}/include
        INTERFACE_LINK_LIBRARIES "gcc_s;util;rt;Threads::Threads;m;${CMAKE_DL_LIBS}")
    add_dependencies(mls_ffi_bridge_fuzz mls_ffi_bridge_fuzz_build)
endif()

ulw_add_library(e2ee_mls src/mls.cpp)
target_include_directories(e2ee_mls PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(e2ee_mls PRIVATE mls_ffi_bridge)
