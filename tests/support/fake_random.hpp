#pragma once

#include "core/ports/random.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ulw::test {

// splitmix64.
class FakeRandom final : public core::ports::IRandom {
public:
    explicit FakeRandom(std::uint64_t seed = 1) noexcept : state_(seed) {}

    void fill(std::span<std::byte> out) noexcept override {
        for (auto& b : out) {
            b = static_cast<std::byte>(next() & 0xFFU);
        }
    }

private:
    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    std::uint64_t state_;
};

} // namespace ulw::test
