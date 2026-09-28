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
    unit/core/ids_test.cpp
    unit/core/storage_key_test.cpp
    unit/core/upload_test.cpp
    unit/core/uuid_test.cpp
    unit/core/video_test.cpp)
target_link_libraries(core_unit_tests PRIVATE ulw::core_domain)
target_include_directories(core_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(core_unit_tests PRIVATE ULW_EXPECTED_VERSION="${PROJECT_VERSION}")

ulw_add_test_suite(os_unit_tests unit
    unit/os/system_random_test.cpp
    unit/os/unique_fd_test.cpp)
target_link_libraries(os_unit_tests PRIVATE ulw::os)
