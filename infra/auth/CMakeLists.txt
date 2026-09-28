ulw_add_library(auth
    src/base64url.cpp
    src/claims.cpp
    src/jwk.cpp
    src/jws.cpp
    src/local_verifier.cpp
    src/signature.cpp)
target_include_directories(auth PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(auth PUBLIC ulw::core_domain PRIVATE OpenSSL::Crypto)
