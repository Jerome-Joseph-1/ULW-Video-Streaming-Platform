#pragma once

#include "core/util/uuid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <libpq-fe.h>
#include <string_view>

namespace infra::postgres {

// Built-in type OIDs are fixed by the system catalog and never renumbered; libpq ships no header
// that names them.
inline constexpr Oid kBoolOid = 16;
inline constexpr Oid kInt8Oid = 20;
inline constexpr Oid kTextOid = 25;
inline constexpr Oid kUuidOid = 2950;

// Statement parameters, sent in binary with explicit types. Integers and UUIDs skip a decimal
// or hex round trip, text needs no terminating NUL (so a view binds without a copy), and the
// server never infers a parameter's type from its context.
class Params {
public:
    // The widest statement, the upload insert, binds ten.
    static constexpr std::size_t kMax = 10;

    Params& add_int(std::int64_t value) noexcept;
    Params& add_bool(bool value) noexcept;
    Params& add_uuid(const core::Uuid& value) noexcept;
    // Borrowed: the viewed bytes must stay put until the statement has been sent.
    Params& add_text(std::string_view value) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    // The arrays PQsendQueryParams and PQexecParams take. They point into the Params they came
    // from, which must neither move nor die before the call.
    struct Wire {
        std::array<const char*, kMax> values{};
        std::array<int, kMax> lengths{};
        std::array<int, kMax> formats{};
        std::array<Oid, kMax> types{};
        int count = 0;
    };
    [[nodiscard]] Wire wire() const noexcept;

private:
    struct Slot {
        Oid type = 0;
        // Fixed-width values, already in network byte order.
        std::array<char, core::Uuid::kByteLength> bytes{};
        std::size_t length = 0;
        std::string_view text;
    };

    Slot& push(Oid type) noexcept;

    std::array<Slot, kMax> slots_{};
    std::size_t count_ = 0;
};

} // namespace infra::postgres
