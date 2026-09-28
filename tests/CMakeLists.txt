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
    unit/net/reactor_factory_test.cpp
    unit/net/reactor_test.cpp
    unit/net/signals_test.cpp
    unit/net/slab_test.cpp
    unit/net/timing_wheel_test.cpp)
target_link_libraries(net_unit_tests PRIVATE ulw::net_lib ulw::test_echo)
# White-box tests of the reactor internals.
target_include_directories(net_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/net/src ${PROJECT_SOURCE_DIR}/tests)
