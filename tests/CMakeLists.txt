include(GoogleTest)

# One executable per label keeps `ctest -L` meaningful and link times short.
# PROPERTIES after the sources are set on every test the suite discovers.
function(ulw_add_test_suite name label)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "PROPERTIES")
    ulw_add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE GTest::gtest_main GTest::gmock)
    gtest_discover_tests(${name}
        PROPERTIES LABELS ${label} ${arg_PROPERTIES}
        DISCOVERY_TIMEOUT 30)
endfunction()

ulw_add_test_suite(core_unit_tests unit
    unit/build_info_test.cpp
    unit/core/content_type_test.cpp
    unit/core/error_messages_test.cpp
    unit/core/hls_test.cpp
    unit/core/ids_test.cpp
    unit/core/json_test.cpp
    unit/core/ladder_test.cpp
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

# A CA and server certificates generated at run time, and the OpenSSL client side of tests.
ulw_add_library(test_tls support/tls_pki.cpp)
target_include_directories(test_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(test_tls PUBLIC ulw::net_lib OpenSSL::SSL)

ulw_add_test_suite(net_unit_tests unit
    unit/net/byte_queue_test.cpp
    unit/net/connect_test.cpp
    unit/net/echo_server_test.cpp
    unit/net/offload_pool_test.cpp
    unit/net/reactor_factory_test.cpp
    unit/net/reactor_test.cpp
    unit/net/signals_test.cpp
    unit/net/slab_test.cpp
    unit/net/timing_wheel_test.cpp
    unit/net/transport_tls_test.cpp)
target_link_libraries(net_unit_tests PRIVATE ulw::net_lib ulw::test_echo ulw::test_tls)
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

ulw_add_test_suite(transfer_conformance_tests conformance
    conformance/object_transfer_conformance_test.cpp)
target_link_libraries(transfer_conformance_tests PRIVATE ulw::storage_fs_transfer)
target_include_directories(transfer_conformance_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
if(ULW_CONFORMANCE_LIVE)
    target_link_libraries(transfer_conformance_tests PRIVATE ulw::storage_s3 ulw::test_live_s3)
    target_compile_definitions(transfer_conformance_tests PRIVATE ULW_CONFORMANCE_LIVE)
endif()
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

ulw_add_executable(hls_fuzz_replay fuzz/hls_fuzz.cpp fuzz/replay_main.cpp)
target_link_libraries(hls_fuzz_replay PRIVATE ulw::core_domain)
add_test(NAME hls_fuzz_corpus
    COMMAND hls_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/hls)
set_tests_properties(hls_fuzz_corpus PROPERTIES LABELS unit)

if(ULW_FUZZ)
    ulw_add_executable(hls_fuzz fuzz/hls_fuzz.cpp)
    target_link_libraries(hls_fuzz PRIVATE ulw::core_domain)
    target_compile_options(hls_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(hls_fuzz PRIVATE -fsanitize=fuzzer)
endif()

ulw_add_test_suite(ws_unit_tests unit
    unit/ws/decoder_test.cpp
    unit/ws/encoder_test.cpp
    unit/ws/handshake_test.cpp
    unit/ws/split_test.cpp
    unit/ws/utf8_test.cpp
    unit/ws/ws_echo_server_test.cpp)
target_link_libraries(ws_unit_tests PRIVATE ulw::codec_ws ulw::test_ws_echo)
target_include_directories(ws_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

# The Autobahn fuzzingclient's peer: tests/autobahn/run.sh.
ulw_add_library(test_ws_echo support/ws_echo_server.cpp)
target_include_directories(test_ws_echo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(test_ws_echo PUBLIC ulw::codec_ws ulw::net_lib)
ulw_add_executable(ulw_ws_echo_server autobahn/ws_echo_main.cpp)
target_link_libraries(ulw_ws_echo_server PRIVATE ulw::test_ws_echo)
# The verdict check run.sh applies to Autobahn's report, on synthetic reports; no docker needed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME autobahn_checker
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/autobahn/check_results_test.py)
set_tests_properties(autobahn_checker PROPERTIES LABELS unit)

# The sandbox scripts' refusal to reach any cluster but the kind sandbox: tests/cluster.
add_test(NAME cluster_guard
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/cluster/guard_test.py)
set_tests_properties(cluster_guard PROPERTIES LABELS unit)

ulw_add_executable(ws_fuzz_replay fuzz/ws_fuzz.cpp fuzz/replay_main.cpp)
target_link_libraries(ws_fuzz_replay PRIVATE ulw::codec_ws)
add_test(NAME ws_fuzz_corpus COMMAND ws_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/ws)
set_tests_properties(ws_fuzz_corpus PROPERTIES LABELS unit)

if(ULW_FUZZ)
    ulw_add_executable(ws_fuzz fuzz/ws_fuzz.cpp)
    target_link_libraries(ws_fuzz PRIVATE ulw::codec_ws)
    target_compile_options(ws_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(ws_fuzz PRIVATE -fsanitize=fuzzer)
endif()
ulw_add_test_suite(rt_unit_tests unit
    unit/rt/recent_keys_test.cpp
    unit/rt/registry_test.cpp
    unit/rt/room_router_test.cpp
    unit/rt/wire_test.cpp)
target_link_libraries(rt_unit_tests PRIVATE ulw::rt_lib)
# The node channel's frame codec is internal to the module.
target_include_directories(rt_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/rt/src)

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
    gateway/config_test.cpp
    gateway/gateway_harness.cpp
    gateway/gateway_playback_test.cpp
    gateway/gateway_tls_test.cpp
    gateway/gateway_upload_test.cpp
    gateway/key_fetcher_test.cpp)
target_link_libraries(gateway_tests PRIVATE
    ulw::gateway_lib ulw::catalog_memory ulw::test_storage_harness ulw::test_http_server
    ulw::test_tls OpenSSL::Crypto)
target_include_directories(gateway_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Not a test: the M9 memory measurement, run by hand (see the file's header).
ulw_add_executable(ulw_gateway_load load/gateway_load.cpp)
target_link_libraries(ulw_gateway_load PRIVATE
    ulw::gateway_lib ulw::catalog_memory ulw::storage_fake ulw::test_tls)
target_include_directories(ulw_gateway_load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
# The pool test docker-pauses that server, so under ctest -j no other test may be using it.
ulw_add_test_suite(postgres_integration_tests integration
    integration/postgres_catalog_test.cpp
    integration/postgres_harness.cpp
    integration/postgres_job_queue_test.cpp
    integration/postgres_migrate_test.cpp
    integration/postgres_pool_test.cpp
    PROPERTIES RESOURCE_LOCK postgres)
target_link_libraries(postgres_integration_tests PRIVATE ulw::pg_store PkgConfig::LIBPQ)
target_include_directories(postgres_integration_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/infra/postgres/src)
target_compile_definitions(postgres_integration_tests
    PRIVATE ULW_MIGRATE_BIN="$<TARGET_FILE:ulw_migrate>")
add_dependencies(postgres_integration_tests ulw_migrate)

# The room registry's store against a live Postgres, as the suite above.
ulw_add_test_suite(room_store_integration_tests integration
    integration/postgres_harness.cpp
    integration/room_store_test.cpp
    PROPERTIES RESOURCE_LOCK postgres)
target_link_libraries(room_store_integration_tests PRIVATE ulw::pg_rooms ulw::pg_store
    PkgConfig::LIBPQ)
target_include_directories(room_store_integration_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/infra/postgres/src)

ulw_add_test_suite(chat_unit_tests unit
    unit/chat/chat_service_test.cpp
    unit/chat/config_test.cpp
    unit/chat/envelope_test.cpp
    unit/chat/session_test.cpp
    unit/chat/token_bucket_test.cpp)
target_link_libraries(chat_unit_tests PRIVATE ulw::chat_lib ulw::os)
target_include_directories(chat_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

# The M16 acceptance: three chat_server processes on the Postgres the suites above use.
ulw_add_test_suite(chat_cluster_tests integration
    integration/chat_cluster_test.cpp
    integration/postgres_harness.cpp
    support/child_process.cpp
    PROPERTIES RESOURCE_LOCK postgres)
target_link_libraries(chat_cluster_tests PRIVATE ulw::pg_store ulw::devtoken ulw::auth ulw::codec_ws
    ulw::net_lib ulw::os PkgConfig::LIBPQ)
target_include_directories(chat_cluster_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/infra/postgres/src)
target_compile_definitions(chat_cluster_tests PRIVATE ULW_CHAT_BIN="$<TARGET_FILE:chat_server>")
add_dependencies(chat_cluster_tests chat_server)

ulw_add_test_suite(auth_unit_tests unit
    unit/auth/base64url_test.cpp
    unit/auth/claims_test.cpp
    unit/auth/devtoken_test.cpp
    unit/auth/jwk_test.cpp
    unit/auth/jwks_verifier_test.cpp
    unit/auth/jws_test.cpp
    unit/auth/local_verifier_test.cpp
    unit/auth/result_cache_test.cpp
    unit/auth/signature_test.cpp
    unit/auth/test_claims.cpp
    unit/auth/test_keys.cpp
    unit/auth/token_extractor_test.cpp)
target_link_libraries(auth_unit_tests PRIVATE ulw::auth ulw::devtoken OpenSSL::Crypto)
# Most suites test the module's internals: parsers, signature and claim checks, verdict cache.
target_include_directories(auth_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/infra/auth/src
    ${PROJECT_SOURCE_DIR}/tests)

if(ULW_BUILD_WORKER)
    ulw_add_test_suite(ffmpeg_unit_tests unit
        unit/ffmpeg/command_test.cpp
        unit/ffmpeg/exit_code_test.cpp
        unit/ffmpeg/hls_rewrite_test.cpp
        unit/ffmpeg/progress_test.cpp
        unit/ffmpeg/sandbox_test.cpp)
    target_link_libraries(ffmpeg_unit_tests PRIVATE ulw::ffmpeg_adapter ulw::os)
    # White-box tests of the adapter internals, and the real sandbox helper.
    target_include_directories(ffmpeg_unit_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/infra/ffmpeg/src ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(ffmpeg_unit_tests
        PRIVATE ULW_SANDBOX_BIN="$<TARGET_FILE:ulw_sandbox>")
    add_dependencies(ffmpeg_unit_tests ulw_sandbox)

    ulw_add_test_suite(worker_unit_tests unit
        unit/worker/config_test.cpp
        unit/worker/heartbeat_test.cpp
        unit/worker/job_runner_test.cpp
        unit/worker/lease_keeper_test.cpp
        unit/worker/workspace_test.cpp)
    target_link_libraries(worker_unit_tests PRIVATE ulw::worker_lib ulw::os)
    target_include_directories(worker_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

    # The adapter on real ffmpeg, and the worker binary against the Postgres and MinIO of
    # deploy/local/compose.yaml.
    ulw_add_test_suite(worker_integration_tests integration
        integration/ffmpeg_transcoder_test.cpp
        integration/postgres_harness.cpp
        integration/worker_test.cpp
        support/child_process.cpp)
    target_link_libraries(worker_integration_tests PRIVATE
        ulw::ffmpeg_adapter ulw::pg_store ulw::storage_fs_transfer ulw::storage_s3
        ulw::test_live_s3 ulw::os PkgConfig::LIBPQ)
    target_include_directories(worker_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/infra/postgres/src ${PROJECT_SOURCE_DIR}/infra/ffmpeg/src)
    target_compile_definitions(worker_integration_tests PRIVATE
        ULW_SANDBOX_BIN="$<TARGET_FILE:ulw_sandbox>"
        ULW_WORKER_BIN="$<TARGET_FILE:transcode_worker>")
    if(ULW_CONFORMANCE_LIVE)
        target_compile_definitions(worker_integration_tests PRIVATE ULW_CONFORMANCE_LIVE)
    endif()
    add_dependencies(worker_integration_tests transcode_worker ulw_sandbox)

    # gateway_server against the Postgres and MinIO of deploy/local/compose.yaml, serving
    # what the worker's ffmpeg command writes.
    ulw_add_test_suite(playback_integration_tests integration
        integration/playback_test.cpp
        integration/postgres_harness.cpp
        support/child_process.cpp
        PROPERTIES RESOURCE_LOCK postgres)
    target_link_libraries(playback_integration_tests PRIVATE
        ulw::ffmpeg_adapter ulw::pg_store ulw::storage_s3 ulw::test_live_s3 ulw::devtoken ulw::os
        PkgConfig::LIBPQ)
    target_include_directories(playback_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/infra/postgres/src ${PROJECT_SOURCE_DIR}/infra/ffmpeg/src)
    target_compile_definitions(playback_integration_tests PRIVATE
        ULW_GATEWAY_BIN="$<TARGET_FILE:gateway_server>")
    if(ULW_CONFORMANCE_LIVE)
        target_compile_definitions(playback_integration_tests PRIVATE ULW_CONFORMANCE_LIVE)
    endif()
    add_dependencies(playback_integration_tests gateway_server)
endif()

ulw_add_test_suite(sdp_unit_tests unit
    unit/sdp/attributes_test.cpp
    unit/sdp/chromium_test.cpp
    unit/sdp/parser_test.cpp
    unit/sdp/property_test.cpp
    unit/sdp/serializer_test.cpp
    unit/sdp/validate_test.cpp)
target_link_libraries(sdp_unit_tests PRIVATE ulw::codec_sdp)
# Chromium's offer and answer, captured by e2e/sdp_roundtrip.spec.mjs.
target_compile_definitions(sdp_unit_tests
    PRIVATE ULW_SDP_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/sdp")

# Parse, validate, serialize, for e2e/sdp_roundtrip.spec.mjs to run Chromium's SDP through.
ulw_add_executable(ulw_sdp_roundtrip media/sdp_roundtrip_main.cpp)
target_link_libraries(ulw_sdp_roundtrip PRIVATE ulw::codec_sdp)

ulw_add_executable(sdp_fuzz_replay fuzz/sdp_fuzz.cpp fuzz/replay_main.cpp)
target_link_libraries(sdp_fuzz_replay PRIVATE ulw::codec_sdp)
add_test(NAME sdp_fuzz_corpus COMMAND sdp_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/sdp)
set_tests_properties(sdp_fuzz_corpus PROPERTIES LABELS unit)

if(ULW_FUZZ)
    ulw_add_executable(sdp_fuzz fuzz/sdp_fuzz.cpp)
    target_link_libraries(sdp_fuzz PRIVATE ulw::codec_sdp)
    target_compile_options(sdp_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(sdp_fuzz PRIVATE -fsanitize=fuzzer)
endif()

ulw_add_test_suite(rtp_unit_tests unit
    unit/rtp/demux_test.cpp
    unit/rtp/pcap_test.cpp
    unit/rtp/rtcp_test.cpp
    unit/rtp/rtp_test.cpp)
target_link_libraries(rtp_unit_tests PRIVATE ulw::codec_rtp)
# The capture and tshark's reading of it, from media/capture_rtp.sh.
target_compile_definitions(rtp_unit_tests
    PRIVATE ULW_RTP_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/rtp")

ulw_add_executable(rtp_fuzz_replay fuzz/rtp_fuzz.cpp fuzz/replay_main.cpp)
target_link_libraries(rtp_fuzz_replay PRIVATE ulw::codec_rtp)
add_test(NAME rtp_fuzz_corpus COMMAND rtp_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/rtp)
set_tests_properties(rtp_fuzz_corpus PROPERTIES LABELS unit)
# The committed tables against tshark's reading of the committed pcap; skipped without tshark.
add_test(NAME rtp_pcap_tshark
    COMMAND bash -c "command -v tshark >/dev/null || exit 77; exec \"$0\" --check"
        ${CMAKE_CURRENT_SOURCE_DIR}/media/capture_rtp.sh)
set_tests_properties(rtp_pcap_tshark PROPERTIES LABELS unit SKIP_RETURN_CODE 77)

if(ULW_FUZZ)
    ulw_add_executable(rtp_fuzz fuzz/rtp_fuzz.cpp)
    target_link_libraries(rtp_fuzz PRIVATE ulw::codec_rtp)
    target_compile_options(rtp_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(rtp_fuzz PRIVATE -fsanitize=fuzzer)
endif()
