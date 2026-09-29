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

Params& Params::add_bytea(std::span<const std::byte> value) noexcept {
    Slot& slot = push(kByteaOid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libpq takes bytes as char.
    slot.borrowed = std::string_view{reinterpret_cast<const char*>(value.data()), value.size()};
    slot.length = value.size();
    return *this;
}

Params& Params::add_bytea_array(std::string_view encoded) noexcept {
    Slot& slot = push(kByteaArrayOid);
    slot.borrowed = encoded;
    slot.length = encoded.size();
    return *this;
}

Params::Wire Params::wire() const noexcept {
    Wire wire;
    for (std::size_t i = 0; i < count_; ++i) {
        const Slot& slot = slots_[i];
        if (slot.type == kTextOid || slot.type == kByteaOid || slot.type == kByteaArrayOid) {
            // A null pointer means SQL NULL, and an empty view may well have one.
            wire.values[i] = slot.borrowed.empty() ? "" : slot.borrowed.data();
        } else {
            wire.values[i] = slot.bytes.data();
        }
        // Every value we bind is far below 2 GiB: titles, keys, reasons, fixed-width numbers and
        // key packages of at most a few KiB each.
        wire.lengths[i] = static_cast<int>(slot.length);
        wire.formats[i] = 1;
        wire.types[i] = slot.type;
    }
    wire.count = static_cast<int>(count_);
    return wire;
}

namespace {

void put_int32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= CHAR_BIT) {
        out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

} // namespace

std::string encode_bytea_array(std::span<const std::vector<std::byte>> elements) {
    // ndim, has-nulls flag, element type, then (length, lower bound) for the dimension.
    constexpr std::size_t kHeader = 5 * sizeof(std::uint32_t);
    std::size_t size = kHeader;
    for (const auto& e : elements) {
        size += sizeof(std::uint32_t) + e.size();
    }
    std::string out;
    out.reserve(size);
    // An empty array has no dimensions at all; a dimension of length 0 is refused.
    put_int32(out, elements.empty() ? 0 : 1);
    put_int32(out, 0);
    put_int32(out, kByteaOid);
    if (!elements.empty()) {
        // Callers bind batches of at most a few hundred small elements.
        put_int32(out, static_cast<std::uint32_t>(elements.size()));
        put_int32(out, 1);
    }
    for (const auto& e : elements) {
        put_int32(out, static_cast<std::uint32_t>(e.size()));
        for (const std::byte b : e) {
            out.push_back(static_cast<char>(b));
        }
    }
    return out;
}

} // namespace infra::postgres
