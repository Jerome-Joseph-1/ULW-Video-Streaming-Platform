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
    unit/storage_key_test.cpp)
target_link_libraries(core_unit_tests PRIVATE ulw::core_domain)
target_compile_definitions(core_unit_tests PRIVATE ULW_EXPECTED_VERSION="${PROJECT_VERSION}")
