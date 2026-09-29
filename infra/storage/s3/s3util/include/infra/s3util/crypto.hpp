#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace infra::s3util {

using Sha256Digest = std::array<unsigned char, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] Sha256Digest sha256(std::string_view data) noexcept;
[[nodiscard]] Sha256Digest hmac_sha256(std::span<const unsigned char> key,
                                       std::string_view data) noexcept;

// SHA-256 of data that arrives in pieces, such as a file too large to hold.
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    Sha256Stream(Sha256Stream&&) noexcept;
    Sha256Stream& operator=(Sha256Stream&&) noexcept;

    void update(std::span<const std::byte> data) noexcept;
    // Once; the stream is spent afterwards.
    [[nodiscard]] Sha256Digest finish() noexcept;

private:
    class Context;
    std::unique_ptr<Context> context_;
};

// Lowercase, as SigV4 wants it everywhere.
[[nodiscard]] std::string to_hex(std::span<const unsigned char> bytes);

} // namespace infra::s3util
