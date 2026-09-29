#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace codec::ws {

// Validates UTF-8 as it arrives, in pieces split anywhere, including inside a sequence. It
// fails on the first byte that no valid continuation could follow, not at the end of the text:
// RFC 3629 excludes overlong forms, the surrogates U+D800-DFFF and anything above U+10FFFF, and
// each shows in the second byte at the latest.
class Utf8Validator {
public:
    // False at the first invalid byte; the validator is then meaningless until reset().
    [[nodiscard]] bool feed(std::span<const std::byte> bytes) noexcept;
    // No sequence is left open.
    [[nodiscard]] bool at_boundary() const noexcept { return need_ == 0; }
    void reset() noexcept { *this = Utf8Validator{}; }

private:
    [[nodiscard]] bool step(std::uint8_t b) noexcept;

    std::uint8_t need_ = 0;
    // The range the next continuation byte must fall in.
    std::uint8_t low_ = 0x80;
    std::uint8_t high_ = 0xBF;
};

[[nodiscard]] bool is_valid_utf8(std::span<const std::byte> bytes) noexcept;

} // namespace codec::ws
