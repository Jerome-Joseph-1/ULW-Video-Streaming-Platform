#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::s3util {

// Key material that is wiped when released. There is deliberately no stream or format
// support, so a secret can only reach a log through an explicit reveal().
class SecretString {
public:
    explicit SecretString(std::string_view value) : bytes_(value.begin(), value.end()) {}
    SecretString(const SecretString&) = default;
    // A vector move hands over the buffer itself, so the source keeps nothing to wipe.
    SecretString(SecretString&&) noexcept = default;
    SecretString& operator=(const SecretString& other);
    SecretString& operator=(SecretString&& other) noexcept;
    ~SecretString();

    [[nodiscard]] std::string_view reveal() const noexcept {
        return {bytes_.data(), bytes_.size()};
    }

private:
    void wipe() noexcept;

    // Not std::string: moving a short string copies it out of the inline buffer and leaves
    // the original bytes behind where the destructor cannot reach them.
    std::vector<char> bytes_;
};

enum class CredentialError : std::uint8_t {
    InvalidAccessKeyId,
    EmptySecret,
    InvalidSessionToken,
};

class Credentials {
public:
    // The access key id and session token end up in a header and a URL, so they are held to
    // characters that cannot break either.
    [[nodiscard]] static std::expected<Credentials, CredentialError>
    make(std::string_view access_key_id, SecretString secret_access_key,
         std::optional<SecretString> session_token = std::nullopt);

    [[nodiscard]] std::string_view access_key_id() const noexcept { return access_key_id_; }
    [[nodiscard]] const SecretString& secret_access_key() const noexcept { return secret_; }
    [[nodiscard]] const std::optional<SecretString>& session_token() const noexcept {
        return session_token_;
    }

private:
    Credentials(std::string access_key_id, SecretString secret,
                std::optional<SecretString> session_token)
        : access_key_id_(std::move(access_key_id)), secret_(std::move(secret)),
          session_token_(std::move(session_token)) {}

    std::string access_key_id_;
    SecretString secret_;
    std::optional<SecretString> session_token_;
};

// Asked for credentials on every signature, from whichever thread is signing, so rotation
// never needs a restart of the component that signs.
class ICredentialProvider {
public:
    virtual ~ICredentialProvider() = default;
    [[nodiscard]] virtual Credentials credentials() const = 0;

protected:
    ICredentialProvider() = default;
    ICredentialProvider(const ICredentialProvider&) = default;
    ICredentialProvider(ICredentialProvider&&) = default;
    ICredentialProvider& operator=(const ICredentialProvider&) = default;
    ICredentialProvider& operator=(ICredentialProvider&&) = default;
};

class StaticCredentialProvider final : public ICredentialProvider {
public:
    explicit StaticCredentialProvider(Credentials credentials)
        : credentials_(std::move(credentials)) {}

    [[nodiscard]] Credentials credentials() const override { return credentials_; }

private:
    Credentials credentials_;
};

struct EnvCredentialNames {
    std::string access_key_id;
    std::string secret_access_key;
    // Optional twice over: a deployment may name no token variable, and a named one may be
    // unset when long-lived keys are in use.
    std::optional<std::string> session_token;
};

struct EnvCredentialError {
    enum class Reason : std::uint8_t { Unset, Malformed };
    Reason reason;
    // Which variable, so a failed start tells the operator what to fix.
    std::string variable;
};

class EnvCredentialProvider final : public ICredentialProvider {
public:
    // Reads the environment here and never again: getenv races with any setenv, and an
    // empty variable counts as unset because that is how most deployment tools clear one.
    [[nodiscard]] static std::expected<EnvCredentialProvider, EnvCredentialError>
    load(const EnvCredentialNames& names);

    [[nodiscard]] Credentials credentials() const override { return credentials_; }

private:
    explicit EnvCredentialProvider(Credentials credentials)
        : credentials_(std::move(credentials)) {}

    Credentials credentials_;
};

} // namespace infra::s3util
