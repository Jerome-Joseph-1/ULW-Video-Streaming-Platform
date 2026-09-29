#pragma once

#include "net/reactor_factory.hpp"

#include "gateway.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gateway {

enum class StorageBackend : std::uint8_t { R2, Minio, Filesystem };

// Plain in the Askedin deployment, where Envoy terminates TLS in front of the gateway
// (ADR-0001); TLS where the gateway faces clients itself.
enum class Transport : std::uint8_t { Plain, Tls };

struct Config {
    std::uint16_t port = 8080;
    net::ReactorKind reactor = net::ReactorKind::IoUring;
    Transport transport = Transport::Plain;
    // Only with Transport::Tls, and then both.
    std::string tls_certificate_chain;
    std::string tls_private_key;
    // Only control calls (create, offset, commit, discard) block, each for one object-store
    // round trip; four keep one slow commit from queueing the rest behind it.
    std::size_t offload_threads = 4;
    StorageBackend storage = StorageBackend::R2;
    // The R2 account id, the MinIO endpoint URL, or the filesystem root.
    std::string storage_location;
    std::string bucket;
    std::string database_url;
    // Exactly one of the two: Askedin's JWKS, or a local key set for offline development.
    std::string jwks_url;
    std::string dev_jwks_file;
    std::string jwt_issuer;
    std::string jwt_audience;
    Limits limits;
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

// Returns the variable's value, or nullopt when it is unset. An empty value counts as unset,
// because that is how most deployment tools clear one.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Everything comes from the environment: arguments are visible to every user through /proc,
// and the connection string carries a password.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

} // namespace gateway
