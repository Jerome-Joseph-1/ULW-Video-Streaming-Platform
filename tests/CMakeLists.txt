include(GoogleTest)

# One executable per label keeps `ctest -L` meaningful and link times short.
function(ulw_add_test_suite name label)
    ulw_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest_main GTest::gmock)
    gtest_discover_tests(${name}
        PROPERTIES LABELS ${label}
        DISCOVERY_TIMEOUT 30)
endfunction()

ulw_add_test_suite(core_unit_tests unit
    unit/build_info_test.cpp
    unit/core/content_type_test.cpp
    unit/core/error_messages_test.cpp
    unit/core/ids_test.cpp
    unit/core/json_test.cpp
    unit/core/parse_test.cpp
    unit/core/storage_key_test.cpp
    unit/core/upload_test.cpp
    unit/core/uuid_test.cpp
    unit/core/video_test.cpp)
target_link_libraries(core_unit_tests PRIVATE ulw::core_domain)
target_include_directories(core_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(core_unit_tests PRIVATE ULW_EXPECTED_VERSION="${PROJECT_VERSION}")

ulw_add_test_suite(os_unit_tests unit
    unit/os/limits_test.cpp
    unit/os/system_random_test.cpp
    unit/os/unique_fd_test.cpp)
target_link_libraries(os_unit_tests PRIVATE ulw::os)

ulw_add_library(test_echo support/echo_server.cpp)
target_include_directories(test_echo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(test_echo PUBLIC ulw::net_lib)

ulw_add_executable(ulw_echo_server load/echo_main.cpp)
target_link_libraries(ulw_echo_server PRIVATE ulw::test_echo)
ulw_add_executable(ulw_loadgen load/loadgen.cpp)

ulw_add_test_suite(net_unit_tests unit
    unit/net/byte_queue_test.cpp
    unit/net/echo_server_test.cpp
    unit/net/offload_pool_test.cpp
    unit/net/reactor_factory_test.cpp
    unit/net/reactor_test.cpp
    unit/net/signals_test.cpp
    unit/net/slab_test.cpp
    unit/net/timing_wheel_test.cpp)
target_link_libraries(net_unit_tests PRIVATE ulw::net_lib ulw::test_echo)
# White-box tests of the reactor internals.
target_include_directories(net_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/net/src ${PROJECT_SOURCE_DIR}/tests)

ulw_add_library(test_storage_harness conformance/storage_harness.cpp)
target_include_directories(test_storage_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance)
target_link_libraries(test_storage_harness PUBLIC ulw::storage_fake ulw::storage_fs ulw::net_lib GTest::gtest)

ulw_add_library(test_live_s3 support/live_s3.cpp)
target_include_directories(test_live_s3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(test_live_s3 PUBLIC ulw::curl_multi ulw::s3util ulw::os)

if(ULW_CONFORMANCE_LIVE)
    target_link_libraries(test_storage_harness PUBLIC ulw::storage_s3 ulw::test_live_s3)
    target_compile_definitions(test_storage_harness PRIVATE ULW_CONFORMANCE_LIVE)
endif()

ulw_add_test_suite(storage_conformance_tests conformance
    conformance/fake_store_faults_test.cpp
    conformance/fs_store_test.cpp
    conformance/storage_conformance_test.cpp)
target_link_libraries(storage_conformance_tests PRIVATE ulw::test_storage_harness)
ulw_add_test_suite(http_unit_tests unit
    unit/http/backpressure_test.cpp
    unit/http/parser_limits_test.cpp
    unit/http/request_framing_test.cpp
    unit/http/request_parser_test.cpp
    unit/http/response_test.cpp
    unit/http/router_test.cpp)
target_link_libraries(http_unit_tests PRIVATE ulw::http_lib)

# Every build replays the committed corpus through the fuzz entry point, so an input that once
# broke the parser fails ctest wherever the unit tests run, not only under libFuzzer.
ulw_add_executable(http_fuzz_replay fuzz/http_fuzz.cpp fuzz/replay_main.cpp)
target_link_libraries(http_fuzz_replay PRIVATE ulw::http_lib)
add_test(NAME http_fuzz_corpus
    COMMAND http_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/http)
set_tests_properties(http_fuzz_corpus PROPERTIES LABELS unit)

if(ULW_FUZZ)
    ulw_add_executable(http_fuzz fuzz/http_fuzz.cpp)
    target_link_libraries(http_fuzz PRIVATE ulw::http_lib)
    target_compile_options(http_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(http_fuzz PRIVATE -fsanitize=fuzzer)
endif()
ulw_add_test_suite(s3util_unit_tests unit
    unit/s3/credentials_test.cpp
    unit/s3/errors_test.cpp
    unit/s3/profile_test.cpp
    unit/s3/retry_test.cpp
    unit/s3/signing_key_test.cpp
    unit/s3/sigv4_test.cpp
    unit/s3/url_test.cpp
    unit/s3/xml_test.cpp)
target_link_libraries(s3util_unit_tests PRIVATE ulw::s3util)
# signing_key_test.cpp checks a private helper directly.
target_include_directories(s3util_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/infra/storage/s3/s3util/src)

ulw_add_test_suite(gateway_tests unit
    gateway/gateway_harness.cpp
    gateway/gateway_upload_test.cpp)
target_link_libraries(gateway_tests PRIVATE
    ulw::gateway_lib ulw::catalog_memory ulw::test_storage_harness OpenSSL::Crypto)
target_include_directories(gateway_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

ulw_add_library(test_http_server support/http_test_server.cpp)
target_include_directories(test_http_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(test_http_server PUBLIC ulw::os PRIVATE ulw::net_lib Threads::Threads)

# Shared, so that its getaddrinfo() comes before libc's for libcurl too. It takes the warning
# flags but not ulw_warnings' link options: -pie means nothing to a shared library.
add_library(test_stalled_resolver SHARED support/stalled_resolver.cpp)
target_compile_features(test_stalled_resolver PRIVATE cxx_std_23)
target_compile_options(test_stalled_resolver
    PRIVATE $<TARGET_PROPERTY:ulw_warnings,INTERFACE_COMPILE_OPTIONS>)
target_link_libraries(test_stalled_resolver PRIVATE ulw_sanitize ${CMAKE_DL_LIBS})

ulw_add_test_suite(curl_unit_tests unit
    unit/curl/fetcher_test.cpp
    unit/curl/http_test.cpp
    unit/curl/multi_test.cpp)
target_link_libraries(curl_unit_tests PRIVATE ulw::curl_multi ulw::test_http_server
    test_stalled_resolver)
target_include_directories(curl_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

ulw_add_test_suite(storage_s3_unit_tests unit
    unit/s3store/part_session_test.cpp
    unit/s3store/s3_store_test.cpp)
target_link_libraries(storage_s3_unit_tests PRIVATE ulw::storage_s3 ulw::test_http_server)
# White-box tests of the adapter's internals.
target_include_directories(storage_s3_unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/infra/storage/s3/src ${PROJECT_SOURCE_DIR}/tests)

ulw_add_test_suite(s3_integration_tests integration
    integration/s3_store_live_test.cpp
    integration/s3_wire_claims_test.cpp)
target_link_libraries(s3_integration_tests PRIVATE ulw::test_live_s3 ulw::core_domain
    ulw::storage_s3)
target_include_directories(s3_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
if(ULW_CONFORMANCE_LIVE)
    target_compile_definitions(s3_integration_tests PRIVATE ULW_CONFORMANCE_LIVE)
endif()

ulw_add_test_suite(postgres_unit_tests unit
    unit/postgres/conninfo_test.cpp
    unit/postgres/lock_key_test.cpp
    unit/postgres/migrations_test.cpp
    unit/postgres/params_test.cpp
    unit/postgres/sqlstate_test.cpp
    unit/postgres/text_test.cpp)
target_link_libraries(postgres_unit_tests PRIVATE ulw::pg_store PkgConfig::LIBPQ)
# White-box tests of the adapter internals.
target_include_directories(postgres_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/infra/postgres/src)
target_compile_definitions(postgres_unit_tests
    PRIVATE ULW_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/migrations")

# Against a live Postgres: ULW_TEST_DATABASE_URL, or the one deploy/local/compose.yaml runs.
ulw_add_test_suite(postgres_integration_tests integration
    integration/postgres_catalog_test.cpp
    integration/postgres_harness.cpp
    integration/postgres_job_queue_test.cpp
    integration/postgres_migrate_test.cpp
    integration/postgres_pool_test.cpp)
target_link_libraries(postgres_integration_tests PRIVATE ulw::pg_store PkgConfig::LIBPQ)
target_include_directories(postgres_integration_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/infra/postgres/src)
target_compile_definitions(postgres_integration_tests
    PRIVATE ULW_MIGRATE_BIN="$<TARGET_FILE:ulw_migrate>")
add_dependencies(postgres_integration_tests ulw_migrate)

ulw_add_test_suite(auth_unit_tests unit
    unit/auth/base64url_test.cpp
    unit/auth/claims_test.cpp
    unit/auth/jwk_test.cpp
    unit/auth/jwks_verifier_test.cpp
    unit/auth/jws_test.cpp
    unit/auth/local_verifier_test.cpp
    unit/auth/result_cache_test.cpp
    unit/auth/signature_test.cpp
    unit/auth/test_claims.cpp
    unit/auth/test_keys.cpp)
target_link_libraries(auth_unit_tests PRIVATE ulw::auth OpenSSL::Crypto)
# White-box tests of the token and key-set parsers.
target_include_directories(auth_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/infra/auth/src
    ${PROJECT_SOURCE_DIR}/tests)
