#pragma once

#include "codec/ws/decoder.hpp"
#include "codec/ws/frame.hpp"
#include "core/ports/random.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ulw::test {

using Bytes = std::vector<std::byte>;

inline Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

inline Bytes text(std::string_view s) {
    Bytes out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

inline Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// A client frame built by hand, so that a test can set any bit a correct encoder never would.
// `first` is the whole first byte: FIN, RSV and opcode.
inline Bytes raw_frame(unsigned first, std::span<const std::byte> payload, bool masked = true) {
    constexpr std::array<unsigned, 4> kKey{0x37, 0xFA, 0x21, 0x3D};
    Bytes out{static_cast<std::byte>(first)};
    const unsigned mask_bit = masked ? 0x80U : 0U;
    const std::uint64_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<std::byte>(mask_bit | n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<std::byte>(mask_bit | 126U));
        out.push_back(static_cast<std::byte>(n >> 8U));
        out.push_back(static_cast<std::byte>(n & 0xFFU));
    } else {
        out.push_back(static_cast<std::byte>(mask_bit | 127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::byte>((n >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }
    if (masked) {
        for (const unsigned k : kKey) {
            out.push_back(static_cast<std::byte>(k));
        }
    }
    std::size_t i = 0;
    for (const std::byte b : payload) {
        const auto key = static_cast<std::byte>(kKey.at(i % kKey.size()));
        out.push_back(masked ? b ^ key : b);
        ++i;
    }
    return out;
}

inline Bytes raw_frame(unsigned first, std::string_view payload, bool masked = true) {
    return raw_frame(first, text(payload), masked);
}

inline codec::ws::Frame message(codec::ws::Opcode op, Bytes payload) {
    return {.opcode = op,
            .fin = true,
            .payload = std::move(payload),
            .close_code = codec::ws::CloseCode::NoStatus};
}

inline codec::ws::Frame close_frame(codec::ws::CloseCode code, std::string_view reason = {}) {
    return {.opcode = codec::ws::Opcode::Close,
            .fin = true,
            .payload = text(reason),
            .close_code = code};
}

// Hands out the bytes it was given, in order and repeating: a test chooses the masking keys.
class FixedKeys final : public core::ports::IRandom {
public:
    explicit FixedKeys(Bytes keys) : keys_(std::move(keys)) {}

    void fill(std::span<std::byte> out) noexcept override {
        for (std::byte& b : out) {
            b = keys_[next_++ % keys_.size()];
        }
    }

private:
    Bytes keys_;
    std::size_t next_ = 0;
};

// Everything a stream decodes to, with the stream fed in pieces of the given sizes (the last
// repeating until the stream runs out).
inline codec::ws::Decoded
decode_in_pieces(std::span<const std::byte> stream, std::span<const std::size_t> sizes,
                 std::uint64_t max_message_bytes = codec::ws::Decoder::kDefaultMaxMessageBytes) {
    codec::ws::Decoder decoder{max_message_bytes};
    codec::ws::Decoded all;
    std::size_t offset = 0;
    std::size_t turn = 0;
    while (offset < stream.size()) {
        const std::size_t size = sizes[std::min(turn++, sizes.size() - 1)];
        const std::size_t n = std::min(size, stream.size() - offset);
        codec::ws::Decoded d = decoder.feed(stream.subspan(offset, n));
        offset += n;
        std::ranges::move(d.frames, std::back_inserter(all.frames));
        if (d.error) {
            all.error = d.error;
            break;
        }
    }
    return all;
}

inline codec::ws::Decoded
decode_whole(std::span<const std::byte> stream,
             std::uint64_t max_message_bytes = codec::ws::Decoder::kDefaultMaxMessageBytes) {
    codec::ws::Decoder decoder{max_message_bytes};
    return decoder.feed(stream);
}

} // namespace ulw::test
