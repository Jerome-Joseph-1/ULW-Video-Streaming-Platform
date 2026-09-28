ulw_add_library(pg_store
    src/params.cpp
    src/result.cpp
    src/sqlstate.cpp
    src/sync_connection.cpp)
target_link_libraries(pg_store PUBLIC ulw::core_domain PRIVATE PkgConfig::LIBPQ)
