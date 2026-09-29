#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace codec::rtp {

// How one fixed-size entry of a packet (a report block, a NACK pair, an SSRC) is laid out.
// Specialised next to each entry type.
template <typename T> struct WireFormat;

template <> struct WireFormat<std::uint32_t> {
    static constexpr std::size_t kSize = 4;
    [[nodiscard]] static std::uint32_t read(std::span<const std::byte, kSize> bytes) noexcept;
};

// A run of fixed-size entries inside a packet, decoded one at a time on access. The parser has
// checked that the run holds a whole number of them.
template <typename T> class Entries {
public:
    static constexpr std::size_t kSize = WireFormat<T>::kSize;

    Entries() noexcept = default;
    explicit Entries(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size() / kSize; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] T operator[](std::size_t i) const noexcept {
        assert(i < size());
        return WireFormat<T>::read(bytes_.subspan(i * kSize).template first<kSize>());
    }

private:
    std::span<const std::byte> bytes_;
};

} // namespace codec::rtp
