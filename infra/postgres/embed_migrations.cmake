# Writes OUTPUT, a C++ source holding every MIGRATIONS_DIR/NNNN_name.sql as a raw string literal.
# Versions must run 1, 2, 3... without gaps: a gap is a lost or misnumbered file.
file(GLOB files RELATIVE ${MIGRATIONS_DIR} ${MIGRATIONS_DIR}/*.sql)
list(SORT files)
if(NOT files)
    message(FATAL_ERROR "no migrations in ${MIGRATIONS_DIR}")
endif()

set(entries "")
set(expected 1)
foreach(file IN LISTS files)
    if(NOT file MATCHES "^([0-9][0-9][0-9][0-9])_([a-z0-9_]+)\\.sql$")
        message(FATAL_ERROR "migration ${file}: expected NNNN_lowercase_name.sql")
    endif()
    set(name ${CMAKE_MATCH_2})
    math(EXPR version "${CMAKE_MATCH_1}")
    if(NOT version EQUAL expected)
        message(FATAL_ERROR "migration ${file}: expected version ${expected}")
    endif()
    file(READ ${MIGRATIONS_DIR}/${file} sql)
    if(sql MATCHES "\\)ulw_sql\"")
        message(FATAL_ERROR "migration ${file}: contains the literal's closing delimiter")
    endif()
    string(APPEND entries
        "    Migration{.version = ${version}, .name = \"${name}\", .sql = R\"ulw_sql(${sql})ulw_sql\"},\n")
    math(EXPR expected "${expected} + 1")
endforeach()

set(content "// Generated from migrations/ by infra/postgres/embed_migrations.cmake.
#include \"infra/postgres/migrator.hpp\"

#include <array>

namespace infra::postgres {

namespace {

constexpr std::array kMigrations{
${entries}};

} // namespace

std::span<const Migration> bundled_migrations() noexcept {
    return kMigrations;
}

} // namespace infra::postgres
")

if(EXISTS ${OUTPUT})
    file(READ ${OUTPUT} previous)
endif()
if(NOT "${previous}" STREQUAL "${content}")
    file(WRITE ${OUTPUT} "${content}")
endif()
