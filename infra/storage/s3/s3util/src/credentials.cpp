#include "infra/s3util/credentials.hpp"

#include "infra/s3util/url.hpp"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <openssl/crypto.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace infra::s3util {

namespace {

// Tokens are base64 in practice; anything visible is fine in a header or an encoded query.
bool is_session_token(std::string_view token) noexcept {
    return !token.empty() && std::ranges::all_of(token, [](char c) { return c > ' ' && c <= '~'; });
}

std::optional<std::string_view> read_env(const std::string& name) {
    // Only reached from EnvCredentialProvider::load, which runs before any signing thread.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* const value = std::getenv(name.c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string_view(value);
}

} // namespace

SecretString& SecretString::operator=(const SecretString& other) {
    if (this != &other) {
        SecretString copy(other);
        *this = std::move(copy);
    }
    return *this;
}

SecretString& SecretString::operator=(SecretString&& other) noexcept {
    if (this != &other) {
        wipe();
        bytes_ = std::move(other.bytes_);
    }
    return *this;
}

SecretString::~SecretString() {
    wipe();
}

void SecretString::wipe() noexcept {
    // OPENSSL_cleanse rather than memset: a store to memory that is about to be freed is
    // dead, and the optimiser is entitled to delete it.
    if (!bytes_.empty()) {
        OPENSSL_cleanse(bytes_.data(), bytes_.size());
    }
}

std::expected<Credentials, CredentialError>
Credentials::make(std::string_view access_key_id, SecretString secret_access_key,
                  std::optional<SecretString> session_token) {
    // 128: the longest access key id IAM issues. Unreserved characters only, so the id reads
    // the same in an Authorization header and in a presigned URL.
    if (access_key_id.empty() || access_key_id.size() > 128 ||
        uri_encode(access_key_id) != access_key_id) {
        return std::unexpected(CredentialError::InvalidAccessKeyId);
    }
    if (secret_access_key.reveal().empty()) {
        return std::unexpected(CredentialError::EmptySecret);
    }
    if (session_token && !is_session_token(session_token->reveal())) {
        return std::unexpected(CredentialError::InvalidSessionToken);
    }
    return Credentials(std::string(access_key_id), std::move(secret_access_key),
                       std::move(session_token));
}

std::expected<EnvCredentialProvider, EnvCredentialError>
EnvCredentialProvider::load(const EnvCredentialNames& names) {
    const auto access_key_id = read_env(names.access_key_id);
    if (!access_key_id) {
        return std::unexpected(EnvCredentialError{.reason = EnvCredentialError::Reason::Unset,
                                                  .variable = names.access_key_id});
    }
    const auto secret = read_env(names.secret_access_key);
    if (!secret) {
        return std::unexpected(EnvCredentialError{.reason = EnvCredentialError::Reason::Unset,
                                                  .variable = names.secret_access_key});
    }
    std::optional<SecretString> token;
    if (names.session_token) {
        if (const auto value = read_env(*names.session_token)) {
            token.emplace(*value);
        }
    }
    auto credentials = Credentials::make(*access_key_id, SecretString(*secret), std::move(token));
    if (!credentials) {
        // read_env never yields an empty secret, so only the id or the token can be at fault.
        const bool token_at_fault = credentials.error() == CredentialError::InvalidSessionToken;
        return std::unexpected(EnvCredentialError{.reason = EnvCredentialError::Reason::Malformed,
                                                  .variable = token_at_fault && names.session_token
                                                                  ? *names.session_token
                                                                  : names.access_key_id});
    }
    return EnvCredentialProvider(std::move(*credentials));
}

} // namespace infra::s3util
