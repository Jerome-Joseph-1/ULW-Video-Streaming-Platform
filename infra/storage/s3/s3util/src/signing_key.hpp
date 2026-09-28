#pragma once

#include "infra/s3util/crypto.hpp"

namespace infra::s3util::detail {

// A derived signing key that is zeroed when it dies and when it is moved from. Moving a bare
// Sha256Digest copies its bytes and leaves the source readable wherever it lived.
class SigningKey {
public:
    // By value: the caller's temporary becomes this parameter, and is wiped before returning.
    explicit SigningKey(Sha256Digest key) noexcept;
    SigningKey(const SigningKey&) = delete;
    SigningKey& operator=(const SigningKey&) = delete;
    SigningKey(SigningKey&& other) noexcept;
    SigningKey& operator=(SigningKey&& other) noexcept;
    ~SigningKey();

    [[nodiscard]] const Sha256Digest& bytes() const noexcept { return key_; }

private:
    Sha256Digest key_;
};

} // namespace infra::s3util::detail
