#include "infra/auth/base64url.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace infra::auth {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::uint8_t kInvalid = 0xFF;

constexpr std::array<std::uint8_t, 256> kDecode = [] {
    std::array<std::uint8_t, 256> table{};
    table.fill(kInvalid);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::uint8_t>(i);
    }
    return table;
}();

} // namespace

std::string encode_base64url(std::span<const unsigned char> bytes) {
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v =
            (std::uint32_t{bytes[i]} << 16U) | (std::uint32_t{bytes[i + 1]} << 8U) | bytes[i + 2];
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[v & 0x3FU]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 0) {
        return out;
    }
    std::uint32_t v = std::uint32_t{bytes[i]} << 16U;
    if (rest == 2) {
        v |= std::uint32_t{bytes[i + 1]} << 8U;
    }
    out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
    if (rest == 2) {
        out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
    }
    return out;
}

std::string encode_base64url(std::string_view bytes) {
    // char and unsigned char may alias each other.
    return encode_base64url({reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
}

std::optional<std::string> decode_base64url(std::string_view text) {
    // A lone trailing character carries 6 bits, less than a byte.
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t acc = 0;
    unsigned bits = 0;
    for (const char c : text) {
        const std::uint8_t v = kDecode[static_cast<unsigned char>(c)];
        if (v == kInvalid) {
            return std::nullopt;
        }
        acc = (acc << 6U) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFFU));
        }
    }
    if ((acc & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return out;
}

} // namespace infra::auth
