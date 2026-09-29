#include "codec/ws/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace codec::ws {

bool Utf8Validator::feed(std::span<const std::byte> bytes) noexcept {
    return std::ranges::all_of(
        bytes, [this](std::byte b) noexcept { return step(std::to_integer<std::uint8_t>(b)); });
}

bool Utf8Validator::step(std::uint8_t b) noexcept {
    if (need_ > 0) {
        if (b < low_ || b > high_) {
            return false;
        }
        --need_;
        low_ = 0x80;
        high_ = 0xBF;
        return true;
    }
    if (b < 0x80) {
        return true;
    }
    // RFC 3629 section 4. The narrowed second-byte ranges exclude, in order: overlong
    // three-byte forms, surrogates, overlong four-byte forms, and code points past U+10FFFF.
    if (b >= 0xC2 && b <= 0xDF) {
        need_ = 1;
    } else if (b == 0xE0) {
        need_ = 2;
        low_ = 0xA0;
    } else if (b == 0xED) {
        need_ = 2;
        high_ = 0x9F;
    } else if (b >= 0xE1 && b <= 0xEF) {
        need_ = 2;
    } else if (b == 0xF0) {
        need_ = 3;
        low_ = 0x90;
    } else if (b == 0xF4) {
        need_ = 3;
        high_ = 0x8F;
    } else if (b >= 0xF1 && b <= 0xF3) {
        need_ = 3;
    } else {
        return false;
    }
    return true;
}

bool is_valid_utf8(std::span<const std::byte> bytes) noexcept {
    Utf8Validator v;
    return v.feed(bytes) && v.at_boundary();
}

} // namespace codec::ws
