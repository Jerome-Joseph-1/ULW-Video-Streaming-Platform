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

if(ULW_FUZZ)
    ulw_add_executable(http_fuzz fuzz/http_fuzz.cpp)
    target_link_libraries(http_fuzz PRIVATE ulw::http_lib)
    target_compile_options(http_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(http_fuzz PRIVATE -fsanitize=fuzzer)
endif()
ulw_add_test_suite(s3util_unit_tests unit
    unit/s3/credentials_test.cpp
    unit/s3/profile_test.cpp
    unit/s3/sigv4_test.cpp
    unit/s3/url_test.cpp)
target_link_libraries(s3util_unit_tests PRIVATE ulw::s3util)
