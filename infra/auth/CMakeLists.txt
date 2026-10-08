ulw_add_library(auth
    src/base64url.cpp
    src/claims.cpp
    src/jwk.cpp
    src/jwks_verifier.cpp
    src/jws.cpp
    src/local_verifier.cpp
    src/result_cache.cpp
    src/service_claim.cpp
    src/signature.cpp
    src/token_extractor.cpp)
target_include_directories(auth PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(auth PUBLIC ulw::core_domain ulw::net_lib PRIVATE OpenSSL::Crypto)
