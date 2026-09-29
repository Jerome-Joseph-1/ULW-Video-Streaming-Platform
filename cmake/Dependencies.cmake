include(FetchContent)

# Every fetched dependency is a tarball committed under third_party/, pinned by SHA-256.
# third_party/README.md records where each one came from.
set(ULW_THIRD_PARTY_DIR ${PROJECT_SOURCE_DIR}/third_party)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(CURL 8.0 REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)

FetchContent_Declare(llhttp
    URL ${ULW_THIRD_PARTY_DIR}/llhttp-9.2.1.tar.gz
    URL_HASH SHA256=3c163891446e529604b590f9ad097b2e98b5ef7e4d3ddcf1cf98b62ca668f23e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
# llhttp's own options default to a shared library; only the static one is linked. Scoped
# to this block so the choice never reaches the cache or any other target. llhttp asks for
# CMake 3.5, under which option() would overwrite these instead of deferring to them.
block()
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    FetchContent_MakeAvailable(llhttp)
endblock()

if(ULW_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL ${ULW_THIRD_PARTY_DIR}/googletest-1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    FetchContent_MakeAvailable(googletest)
endif()

# The live packager terminates SRT itself, so ffmpeg can stay in its empty network namespace
# (ADR-0037). Built static, without its command-line apps, against OpenSSL for the passphrase.
if(ULW_BUILD_WORKER)
    FetchContent_Declare(srt
        URL ${ULW_THIRD_PARTY_DIR}/srt-1.5.4.tar.gz
        URL_HASH SHA256=d0a8b600fe1b4eaaf6277530e3cfc8f15b8ce4035f16af4a5eb5d4b123640cdd
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    # Scoped like llhttp's: nothing leaks into the cache or another target. srt asks for CMake
    # 2.8, which CMake 4 refuses without the policy floor.
    block()
        set(ENABLE_APPS OFF)
        set(ENABLE_SHARED OFF)
        set(ENABLE_STATIC ON)
        set(ENABLE_TESTING OFF)
        set(ENABLE_UNITTESTS OFF)
        set(ENABLE_ENCRYPTION ON)
        set(USE_ENCLIB openssl)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        FetchContent_MakeAvailable(srt)
    endblock()
endif()
