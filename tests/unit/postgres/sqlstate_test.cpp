#include "libpq_handles.hpp"
#include "sqlstate.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using infra::postgres::classify;
using infra::postgres::DbError;
using infra::postgres::error_of;
using infra::postgres::ResultHandle;

struct Case {
    std::string_view sqlstate;
    DbError error;
};

using enum DbError;

constexpr std::array kTable{
    Case{.sqlstate = "23505", .error = Duplicate},
    Case{.sqlstate = "40001", .error = Retry},
    Case{.sqlstate = "40P01", .error = Retry},
    Case{.sqlstate = "08000", .error = ConnectionLost},
    Case{.sqlstate = "08001", .error = ConnectionLost},
    Case{.sqlstate = "08006", .error = ConnectionLost},
    Case{.sqlstate = "08P01", .error = ConnectionLost},
    Case{.sqlstate = "57P01", .error = ConnectionLost},
    Case{.sqlstate = "57P02", .error = ConnectionLost},
    Case{.sqlstate = "57P03", .error = ConnectionLost},
    Case{.sqlstate = "57P05", .error = ConnectionLost},
    Case{.sqlstate = "57014", .error = Timeout},
    Case{.sqlstate = "55P03", .error = LockTimeout},
    Case{.sqlstate = "23502", .error = Constraint},
    Case{.sqlstate = "23503", .error = Constraint},
    Case{.sqlstate = "23514", .error = Constraint},
    Case{.sqlstate = "40003", .error = Rejected},
    Case{.sqlstate = "42601", .error = Rejected},
    Case{.sqlstate = "42P01", .error = Rejected},
    Case{.sqlstate = "22003", .error = Rejected},
    Case{.sqlstate = "53300", .error = Rejected},
    Case{.sqlstate = "55000", .error = Rejected},
    Case{.sqlstate = "", .error = Rejected},
    Case{.sqlstate = "2350", .error = Rejected},
    Case{.sqlstate = "235050", .error = Rejected},
};

TEST(Classify, MapsEverySqlstateInTheTable) {
    for (const Case& c : kTable) {
        EXPECT_EQ(classify(c.sqlstate), c.error) << "sqlstate '" << c.sqlstate << "'";
    }
}

TEST(ErrorOf, ErrorWithoutSqlstateOnADeadConnectionIsConnectionLost) {
    // libpq builds such results itself when the socket fails; a null connection reads as bad.
    const ResultHandle result{PQmakeEmptyPGresult(nullptr, PGRES_FATAL_ERROR)};
    ASSERT_TRUE(result);
    EXPECT_EQ(error_of(result.get(), nullptr), ConnectionLost);
    EXPECT_EQ(error_of(nullptr, nullptr), ConnectionLost);
}

} // namespace
