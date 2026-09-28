#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "os/system_clock.hpp"
#include "os/unique_fd.hpp"

#include "devtoken/dev_key.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::string_view kUsage = R"(usage:
  ulw_devtoken keygen <key-file>
      Writes a new Ed25519 private key to <key-file>, mode 0600. Never overwrites.
  ulw_devtoken jwks <key-file>
      Prints the public JWK set to hand the service's local verifier.
  ulw_devtoken mint <key-file> --iss <issuer> --sub <subject>
                    [--aud <audience>] [--email <email>] [--ttl <seconds>]
      Prints a token signed with the key. --aud defaults to askedin-platform,
      --ttl to 3600.
)";

// JWT_AUDIENCE's default (ADR-0018).
constexpr std::string_view kDefaultAudience = "askedin-platform";
constexpr std::int64_t kDefaultTtlSeconds = 3600;
// A week covers a test environment left running over a weekend; a longer-lived token is one
// that ends up pasted somewhere it outlives its purpose.
constexpr std::int64_t kMaxTtlSeconds = std::int64_t{7} * 24 * 3600;
// A private JWK is about 200 bytes; anything much larger is not one.
constexpr std::size_t kMaxKeyFileBytes = 4096;

constexpr int kUsageError = 2;

int fail(std::string_view what) {
    std::println(stderr, "ulw_devtoken: {}", what);
    return 1;
}

int fail(std::string_view what, std::string_view path, int err) {
    std::println(stderr, "ulw_devtoken: {} {}: {}", what, path,
                 std::error_code(err, std::generic_category()).message());
    return 1;
}

int usage() {
    std::print(stderr, "{}", kUsage);
    return kUsageError;
}

std::expected<std::string, int> read_key_file(const std::string& path) {
    const os::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!fd) {
        return std::unexpected(errno);
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return std::unexpected(errno);
    }
    // As ssh does with a private key: one that others can read has already leaked.
    if (!S_ISREG(st.st_mode) || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(EPERM);
    }
    std::string contents(kMaxKeyFileBytes + 1, '\0');
    std::size_t filled = 0;
    while (filled < contents.size()) {
        const ssize_t n = ::read(fd.get(), contents.data() + filled, contents.size() - filled);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return std::unexpected(errno);
        }
        if (n == 0) {
            break;
        }
        filled += static_cast<std::size_t>(n);
    }
    if (filled > kMaxKeyFileBytes) {
        return std::unexpected(EFBIG);
    }
    contents.resize(filled);
    return contents;
}

std::optional<devtoken::DevKey> load_key(const std::string& path) {
    const std::expected<std::string, int> text = read_key_file(path);
    if (!text && text.error() == EPERM) {
        fail(path + ": key file must be a regular file with mode 0600");
        return std::nullopt;
    }
    if (!text) {
        fail("cannot read key file", path, text.error());
        return std::nullopt;
    }
    std::expected<devtoken::DevKey, devtoken::DevKeyError> key =
        devtoken::DevKey::from_private_jwk(*text);
    if (!key) {
        fail(path + ": " + std::string(devtoken::to_string(key.error())));
        return std::nullopt;
    }
    return std::move(*key);
}

int keygen(const std::string& path) {
    const std::expected<devtoken::DevKey, devtoken::DevKeyError> key = devtoken::DevKey::generate();
    if (!key) {
        return fail(devtoken::to_string(key.error()));
    }
    const std::expected<std::string, devtoken::DevKeyError> jwk = key->private_jwk();
    if (!jwk) {
        return fail(devtoken::to_string(jwk.error()));
    }
    // O_EXCL: an existing key is never replaced, since tokens and key sets made from it would
    // silently stop verifying.
    const os::UniqueFd fd{
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!fd) {
        return fail("cannot create", path, errno);
    }
    std::size_t written = 0;
    while (written < jwk->size()) {
        const ssize_t n = ::write(fd.get(), jwk->data() + written, jwk->size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            // A torn key file would block the next keygen and load as nothing.
            ::unlink(path.c_str());
            return fail("cannot write", path, err);
        }
        written += static_cast<std::size_t>(n);
    }
    std::println("{}: key {}", path, key->kid());
    return 0;
}

int jwks(const std::string& path) {
    const std::optional<devtoken::DevKey> key = load_key(path);
    if (!key) {
        return 1;
    }
    std::print("{}", key->public_jwks());
    return 0;
}

std::optional<std::int64_t> parse_ttl(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 1 ||
        value > kMaxTtlSeconds) {
        return std::nullopt;
    }
    return value;
}

int mint(const std::string& path, std::span<const std::string_view> options) {
    devtoken::MintRequest request{.issuer = {},
                                  .audience = std::string(kDefaultAudience),
                                  .subject = {},
                                  .email = {},
                                  .ttl = core::Seconds{kDefaultTtlSeconds}};
    for (std::size_t i = 0; i < options.size(); i += 2) {
        if (i + 1 == options.size()) {
            return usage();
        }
        const std::string_view name = options[i];
        const std::string_view value = options[i + 1];
        if (name == "--iss") {
            request.issuer = value;
        } else if (name == "--sub") {
            request.subject = value;
        } else if (name == "--aud") {
            request.audience = value;
        } else if (name == "--email") {
            request.email = value;
        } else if (name == "--ttl") {
            const std::optional<std::int64_t> ttl = parse_ttl(value);
            if (!ttl) {
                return fail(std::format("--ttl takes whole seconds from 1 to {}", kMaxTtlSeconds));
            }
            request.ttl = core::Seconds{*ttl};
        } else {
            return usage();
        }
    }
    if (request.issuer.empty() || request.subject.empty() || request.audience.empty()) {
        return usage();
    }
    // The verifier takes subjects in UserId form only, so a token it would refuse is not made.
    if (!core::UserId::parse(request.subject)) {
        return fail("--sub is 1 to 128 of [A-Za-z0-9._:@|+-]");
    }
    const std::optional<devtoken::DevKey> key = load_key(path);
    if (!key) {
        return 1;
    }
    const os::SystemClock clock;
    const std::expected<std::string, devtoken::DevKeyError> token =
        key->mint(request, clock.wall_now());
    if (!token) {
        return fail(devtoken::to_string(token.error()));
    }
    std::println("{}", *token);
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args(raw.begin() + (raw.empty() ? 0 : 1), raw.end());
    if (args.size() < 2) {
        return usage();
    }
    const std::string path(args[1]);
    const std::span<const std::string_view> rest = std::span(args).subspan(2);
    if (args[0] == "keygen" && rest.empty()) {
        return keygen(path);
    }
    if (args[0] == "jwks" && rest.empty()) {
        return jwks(path);
    }
    if (args[0] == "mint") {
        return mint(path, rest);
    }
    return usage();
} catch (...) {
    static_cast<void>(std::fputs("ulw_devtoken: failed to write output\n", stderr));
    return 1;
}
