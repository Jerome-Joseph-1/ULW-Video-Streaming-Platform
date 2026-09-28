ulw_add_executable(ulw_migrate src/main.cpp)
target_link_libraries(ulw_migrate PRIVATE ulw::pg_store)
