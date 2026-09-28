#include "infra/s3util/credentials.hpp"

#include <expected>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <ostream>
// <cstdlib> promises only the ISO C functions; setenv and unsetenv are POSIX.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers)
#include <string>
#include <string_view>
#include <utility>

namespace {

using infra::s3util::CredentialError;
using infra::s3util::Credentials;
using infra::s3util::EnvCredentialError;
using infra::s3util::EnvCredentialNames;
using infra::s3util::EnvCredentialProvider;
using infra::s3util::SecretString;
using infra::s3util::StaticCredentialProvider;

template <typename T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };

static_assert(!Streamable<SecretString>, "a secret must not be streamable into a log line");
static_assert(!std::formattable<SecretString, char>,
              "a secret must not be formattable into a log line");

// Tests run on one thread, which is what makes touching the environment safe here.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value) : name_(std::move(name)) {
        if (value == nullptr) {
            ::unsetenv(name_.c_str()); // NOLINT(concurrency-mt-unsafe)
        } else {
            ::setenv(name_.c_str(), value, 1); // NOLINT(concurrency-mt-unsafe)
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;
    ~ScopedEnv() { ::unsetenv(name_.c_str()); } // NOLINT(concurrency-mt-unsafe)

private:
    std::string name_;
};

constexpr const char* kIdVar = "ULW_TEST_S3_ACCESS_KEY_ID";
constexpr const char* kSecretVar = "ULW_TEST_S3_SECRET_ACCESS_KEY";
constexpr const char* kTokenVar = "ULW_TEST_S3_SESSION_TOKEN";

EnvCredentialNames names() {
    return {.access_key_id = kIdVar, .secret_access_key = kSecretVar, .session_token = kTokenVar};
}

std::optional<std::string> token_of(const Credentials& c) {
    return c.session_token().transform(
        [](const SecretString& t) { return std::string(t.reveal()); });
}

TEST(SecretString, CopiesAndAssignmentsAreIndependentOfTheirSource) {
    const SecretString original("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    SecretString copy(original);
    EXPECT_EQ(copy.reveal(), original.reveal());

    SecretString assigned("short");
    assigned = original;
    EXPECT_EQ(assigned.reveal(), original.reveal());

    copy = SecretString("rotated-secret");
    EXPECT_EQ(copy.reveal(), "rotated-secret");
    EXPECT_EQ(original.reveal(), "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    EXPECT_EQ(assigned.reveal(), "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
}

TEST(Credentials, AcceptsTheKeyShapesOfEveryBackend) {
    for (const std::string_view id : {"AKIAIOSFODNN7EXAMPLE", "0123456789abcdef0123456789abcdef",
                                      "minioadmin", "0045f0e1d2c3b4a0000000001"}) {
        const auto c = Credentials::make(id, SecretString("secret"));
        ASSERT_TRUE(c.has_value()) << id;
        EXPECT_EQ(c->access_key_id(), id);
        EXPECT_EQ(c->secret_access_key().reveal(), "secret");
        EXPECT_EQ(token_of(*c), std::nullopt);
    }
}

TEST(Credentials, RejectsAccessKeyIdsThatWouldBreakTheCredentialScope) {
    for (const std::string_view id : {"", "AKIA/EXAMPLE", "AKIA,EXAMPLE", "AKIA EXAMPLE",
                                      "AKIA=EXAMPLE", "AKIA\r\nX-Evil: 1", "AKIA%2F"}) {
        EXPECT_EQ(Credentials::make(id, SecretString("secret")),
                  std::unexpected(CredentialError::InvalidAccessKeyId))
            << id;
    }
    EXPECT_TRUE(Credentials::make(std::string(128, 'A'), SecretString("s")).has_value());
    EXPECT_FALSE(Credentials::make(std::string(129, 'A'), SecretString("s")).has_value());
}

TEST(Credentials, RejectsAnEmptySecretAndAnUnsendableSessionToken) {
    EXPECT_EQ(Credentials::make("AKIAIOSFODNN7EXAMPLE", SecretString("")),
              std::unexpected(CredentialError::EmptySecret));
    for (const std::string_view token : {"", "has space", "line\nbreak", "tab\there"}) {
        EXPECT_EQ(Credentials::make("AKIAIOSFODNN7EXAMPLE", SecretString("s"), SecretString(token)),
                  std::unexpected(CredentialError::InvalidSessionToken))
            << token;
    }
    const auto c = Credentials::make("AKIAIOSFODNN7EXAMPLE", SecretString("s"),
                                     SecretString("FwoGZXIvYXdzE+/abc=="));
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(token_of(*c), "FwoGZXIvYXdzE+/abc==");
}

TEST(StaticCredentialProvider, HandsOutTheCredentialsItWasGiven) {
    const StaticCredentialProvider provider(
        Credentials::make("AKIAIOSFODNN7EXAMPLE", SecretString("secret")).value());
    const auto c = provider.credentials();
    EXPECT_EQ(c.access_key_id(), "AKIAIOSFODNN7EXAMPLE");
    EXPECT_EQ(c.secret_access_key().reveal(), "secret");
}

TEST(EnvCredentialProvider, ReadsTheNamedVariablesOnceAtLoad) {
    const ScopedEnv id(kIdVar, "AKIAIOSFODNN7EXAMPLE");
    const ScopedEnv secret(kSecretVar, "first-secret");
    const ScopedEnv token(kTokenVar, nullptr);

    const auto provider = EnvCredentialProvider::load(names());
    ASSERT_TRUE(provider.has_value());

    const ScopedEnv rotated(kIdVar, "AKIAROTATEDEXAMPLE");
    const auto c = provider->credentials();
    EXPECT_EQ(c.access_key_id(), "AKIAIOSFODNN7EXAMPLE");
    EXPECT_EQ(c.secret_access_key().reveal(), "first-secret");
    EXPECT_EQ(token_of(c), std::nullopt);
}

TEST(EnvCredentialProvider, PicksUpASessionTokenWhenItsVariableIsSet) {
    const ScopedEnv id(kIdVar, "ASIAIOSFODNN7EXAMPLE");
    const ScopedEnv secret(kSecretVar, "secret");
    const ScopedEnv token(kTokenVar, "FwoGZXIvYXdzE+/abc==");

    const auto provider = EnvCredentialProvider::load(names());
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(token_of(provider->credentials()), "FwoGZXIvYXdzE+/abc==");
}

TEST(EnvCredentialProvider, IgnoresTheTokenWhenNoTokenVariableIsNamed) {
    const ScopedEnv id(kIdVar, "AKIAIOSFODNN7EXAMPLE");
    const ScopedEnv secret(kSecretVar, "secret");
    const ScopedEnv token(kTokenVar, "FwoGZXIvYXdzE+/abc==");

    auto without_token = names();
    without_token.session_token.reset();
    const auto provider = EnvCredentialProvider::load(without_token);
    ASSERT_TRUE(provider.has_value());
    EXPECT_EQ(token_of(provider->credentials()), std::nullopt);
}

TEST(EnvCredentialProvider, NamesTheVariableThatIsUnsetOrEmpty) {
    {
        const ScopedEnv id(kIdVar, nullptr);
        const ScopedEnv secret(kSecretVar, "secret");
        const auto r = EnvCredentialProvider::load(names());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().reason, EnvCredentialError::Reason::Unset);
        EXPECT_EQ(r.error().variable, kIdVar);
    }
    {
        const ScopedEnv id(kIdVar, "AKIAIOSFODNN7EXAMPLE");
        const ScopedEnv secret(kSecretVar, "");
        const auto r = EnvCredentialProvider::load(names());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().reason, EnvCredentialError::Reason::Unset);
        EXPECT_EQ(r.error().variable, kSecretVar);
    }
}

TEST(EnvCredentialProvider, NamesTheVariableThatIsMalformed) {
    const ScopedEnv secret(kSecretVar, "secret");
    {
        const ScopedEnv id(kIdVar, "AKIA/EXAMPLE");
        const ScopedEnv token(kTokenVar, nullptr);
        const auto r = EnvCredentialProvider::load(names());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().reason, EnvCredentialError::Reason::Malformed);
        EXPECT_EQ(r.error().variable, kIdVar);
    }
    {
        const ScopedEnv id(kIdVar, "AKIAIOSFODNN7EXAMPLE");
        const ScopedEnv token(kTokenVar, "two words");
        const auto r = EnvCredentialProvider::load(names());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().reason, EnvCredentialError::Reason::Malformed);
        EXPECT_EQ(r.error().variable, kTokenVar);
    }
}

} // namespace
