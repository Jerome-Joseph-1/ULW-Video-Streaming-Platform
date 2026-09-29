#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ulw::test {

// Builds packets field by field, in network byte order.
class Wire {
public:
    Wire& u8(unsigned v) {
        bytes_.push_back(static_cast<std::byte>(v & 0xFFU));
        return *this;
    }

    Wire& u16(unsigned v) { return u8(v >> 8U).u8(v); }
    Wire& u24(std::uint32_t v) { return u8(v >> 16U).u16(v & 0xFFFFU); }
    Wire& u32(std::uint32_t v) { return u16(v >> 16U).u16(v & 0xFFFFU); }
    Wire& u64(std::uint64_t v) {
        return u32(static_cast<std::uint32_t>(v >> 32U)).u32(static_cast<std::uint32_t>(v));
    }

    Wire& raw(std::initializer_list<unsigned> values) {
        for (const unsigned v : values) {
            u8(v);
        }
        return *this;
    }

    Wire& text(std::string_view s) {
        for (const char c : s) {
            u8(static_cast<unsigned char>(c));
        }
        return *this;
    }

    Wire& append(std::span<const std::byte> more) {
        bytes_.insert(bytes_.end(), more.begin(), more.end());
        return *this;
    }

    // An RTCP common header whose length field covers `body_bytes` after it.
    Wire& rtcp_header(unsigned count, unsigned type, std::size_t body_bytes, bool padding = false) {
        return u8(0x80U | (padding ? 0x20U : 0U) | count)
            .u8(type)
            .u16(static_cast<unsigned>(body_bytes / 4));
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

[[nodiscard]] inline std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    return Wire{}.raw(values).take();
}

[[nodiscard]] inline std::string_view as_text(std::span<const std::byte> bytes) {
    // Test output only: the bytes are ASCII the test wrote.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace ulw::test
