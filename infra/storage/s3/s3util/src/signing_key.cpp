#include "signing_key.hpp"

#include "infra/s3util/crypto.hpp"

#include <openssl/crypto.h>

namespace infra::s3util::detail {

namespace {

void wipe(Sha256Digest& key) noexcept {
    OPENSSL_cleanse(key.data(), key.size());
}

} // namespace

SigningKey::SigningKey(Sha256Digest key) noexcept : key_(key) {
    wipe(key);
}

SigningKey::SigningKey(SigningKey&& other) noexcept : key_(other.key_) {
    wipe(other.key_);
}

SigningKey& SigningKey::operator=(SigningKey&& other) noexcept {
    if (this != &other) {
        key_ = other.key_;
        wipe(other.key_);
    }
    return *this;
}

SigningKey::~SigningKey() {
    wipe(key_);
}

} // namespace infra::s3util::detail
