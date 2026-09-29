file(GLOB ulw_migration_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/migrations/*.sql)
set(ulw_bundled_migrations ${CMAKE_CURRENT_BINARY_DIR}/bundled_migrations.cpp)
add_custom_command(
    OUTPUT ${ulw_bundled_migrations}
    COMMAND ${CMAKE_COMMAND}
        -DMIGRATIONS_DIR=${PROJECT_SOURCE_DIR}/migrations
        -DOUTPUT=${ulw_bundled_migrations}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_migrations.cmake
    DEPENDS ${ulw_migration_files} ${CMAKE_CURRENT_SOURCE_DIR}/embed_migrations.cmake
    COMMENT "Embedding schema migrations"
    VERBATIM)

ulw_add_library(pg_store
    src/conninfo.cpp
    src/e2ee_directory.cpp
    src/health_check.cpp
    src/job_queue.cpp
    src/message_store.cpp
    src/migrator.cpp
    src/params.cpp
    src/pool.cpp
    src/result.cpp
    src/sqlstate.cpp
    src/sync_connection.cpp
    src/upload_catalog.cpp
    src/upload_reaper.cpp
    ${ulw_bundled_migrations})
target_include_directories(pg_store PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(pg_store
    PUBLIC ulw::core_domain ulw::net_lib
    PRIVATE PkgConfig::LIBPQ)

# The room registry's store, apart from pg_store so that only chat_server links the realtime
# runtime. It drives the same pool, from the same sources.
ulw_add_library(pg_rooms
    src/room_store.cpp)
target_include_directories(pg_rooms PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(pg_rooms
    PUBLIC ulw::rt_lib ulw::net_lib
    PRIVATE ulw::pg_store PkgConfig::LIBPQ)
