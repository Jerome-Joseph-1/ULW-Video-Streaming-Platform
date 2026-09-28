include(FetchContent)

# Every fetched dependency is a tarball committed under third_party/, pinned by SHA-256.
# third_party/README.md records where each one came from.
set(ULW_THIRD_PARTY_DIR ${PROJECT_SOURCE_DIR}/third_party)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# llhttp's own options default to a shared library; only the static one is linked.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(llhttp
    URL ${ULW_THIRD_PARTY_DIR}/llhttp-9.2.1.tar.gz
    URL_HASH SHA256=3c163891446e529604b590f9ad097b2e98b5ef7e4d3ddcf1cf98b62ca668f23e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(llhttp)
find_package(OpenSSL 3.0 REQUIRED)

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
