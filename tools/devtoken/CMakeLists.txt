ulw_add_library(devtoken src/dev_key.cpp)
target_include_directories(devtoken PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(devtoken PUBLIC ulw::core_domain PRIVATE ulw::auth OpenSSL::Crypto)

ulw_add_executable(ulw_devtoken src/main.cpp)
target_link_libraries(ulw_devtoken PRIVATE ulw::devtoken ulw::os)
