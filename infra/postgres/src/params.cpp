#include "params.hpp"

#include <climits>
#include <cstdlib>

namespace infra::postgres {

Params::Slot& Params::push(Oid type) noexcept {
    if (count_ == kMax) {
        // A statement with more parameters than any we write is a bug, not a runtime condition.
        std::abort();
    }
    Slot& slot = slots_[count_++];
    slot = Slot{};
    slot.type = type;
    return slot;
}

Params& Params::add_int(std::int64_t value) noexcept {
    Slot& slot = push(kInt8Oid);
    const auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < sizeof bits; ++i) {
        slot.bytes[i] = static_cast<char>((bits >> (CHAR_BIT * (sizeof bits - 1 - i))) & 0xFFU);
    }
    slot.length = sizeof bits;
    return *this;
}

Params& Params::add_bool(bool value) noexcept {
    Slot& slot = push(kBoolOid);
    slot.bytes[0] = value ? 1 : 0;
    slot.length = 1;
    return *this;
}

Params& Params::add_uuid(const core::Uuid& value) noexcept {
    Slot& slot = push(kUuidOid);
    const auto bytes = value.bytes();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        slot.bytes[i] = static_cast<char>(bytes[i]);
    }
    slot.length = bytes.size();
    return *this;
}

Params& Params::add_text(std::string_view value) noexcept {
    Slot& slot = push(kTextOid);
    slot.borrowed = value;
    slot.length = value.size();
    return *this;
}

Params& Params::add_bytes(std::span<const std::byte> value) noexcept {
    Slot& slot = push(kByteaOid);
    // libpq takes every value as char; std::byte and char may alias each other.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    slot.borrowed = {reinterpret_cast<const char*>(value.data()), value.size()};
    slot.length = value.size();
    return *this;
}

Params::Wire Params::wire() const noexcept {
    Wire wire;
    for (std::size_t i = 0; i < count_; ++i) {
        const Slot& slot = slots_[i];
        if (slot.type == kTextOid || slot.type == kByteaOid) {
            // A null pointer means SQL NULL, and an empty view may well have one.
            wire.values[i] = slot.borrowed.empty() ? "" : slot.borrowed.data();
        } else {
            wire.values[i] = slot.bytes.data();
        }
        // Every value we bind is far below 2 GiB: titles, keys, reasons, fixed-width numbers,
        // message bodies of at most 64 KiB.
        wire.lengths[i] = static_cast<int>(slot.length);
        wire.formats[i] = 1;
        wire.types[i] = slot.type;
    }
    wire.count = static_cast<int>(count_);
    return wire;
}

} // namespace infra::postgres
