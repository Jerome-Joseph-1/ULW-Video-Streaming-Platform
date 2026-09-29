#pragma once

#include "net/reactor_factory.hpp"

#include "gateway.hpp"
#include "ops/log.hpp"
#include "ops/settings.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
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
    // Its contents, read and checked by load_config.
    std::string dev_jwks;
    std::string jwt_issuer;
    std::string jwt_audience;
    // The size every chunk but an upload's last has, and the object store's part size.
    std::uint64_t chunk_size = std::uint64_t{8} << 20U;
    ops::Level log_level = ops::Level::Info;
    // Who to become when started as root; empty stays root, with a warning.
    std::string run_as_user;
    Limits limits;
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

// Returns the variable's value, or nullopt when it is unset. An empty value counts as unset,
// because that is how most deployment tools clear one.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Every value the gateway reads, by the environment variable deployments set it with. The
// connection string carries a password, so it never comes from the command line.
[[nodiscard]] std::span<const ops::Setting> settings() noexcept;

// Reads and checks everything but what depends on the running process; `env` looks values up
// by variable name, from the layered settings or the environment alone.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

// Two descriptors per connection (the client's, and the backend socket its upload holds), and
// 64 for the process's own: stdio, the listener, the reactor's, the pools' wakeups, the
// database sessions. An admission limit the descriptor limit cannot back would fail with
// EMFILE at the worst moment instead of answering 503.
[[nodiscard]] std::expected<void, ConfigError> check_descriptor_budget(const Limits& limits,
                                                                       std::size_t nofile);

// One line per value, secrets redacted, saying where each came from.
void log_effective(const Config& config, const ops::Settings& layers, ops::Logger& log);

} // namespace gateway
