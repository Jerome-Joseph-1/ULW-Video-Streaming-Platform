#pragma once

#include "net/reactor_factory.hpp"

#include "gateway.hpp"
#include "ops/log.hpp"
#include "ops/settings.hpp"
#include "publisher_watch.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gateway {

enum class StorageBackend : std::uint8_t { R2, Minio, Filesystem };

// Plain in the Kubernetes deployment, where Envoy terminates TLS in front of the gateway
// (ADR-0001); TLS where the gateway faces clients itself.
enum class Transport : std::uint8_t { Plain, Tls };

// Where a stream's packager runs (ADR-0092): a child process of the gateway, for development
// and the local stack, or a Kubernetes Job.
enum class PackagerRuntime : std::uint8_t { Process, Kubernetes };

// The stream service's settings. Live publishing is on when LIVEKIT_API_URL is set, and
// everything it needs must be set with it.
struct LiveConfig {
    bool enabled = false;
    std::string livekit_api_url;
    std::string livekit_client_url;
    std::string livekit_api_key;
    std::string livekit_api_secret;
    // Where the relay calls a stream's packager: srt://host:port, "{stream}" allowed in the host.
    std::string packager_srt;
    PackagerRuntime runtime = PackagerRuntime::Process;
    // Process: the live_packager binary, and the environment each one starts with.
    std::string packager_binary;
    std::vector<std::string> packager_environment;
    // Kubernetes: the Job template's text, read at start, and what fills it.
    std::string job_template_file;
    std::string job_template;
    std::string image_tag;
    // ULW_LIVE_PACKAGER_PULL_POLICY and ULW_LIVE_PACKAGER_SECRET: the template's
    // ${IMAGE_PULL_POLICY} and ${LIVE_PACKAGER_SECRET}; its store is the gateway's own.
    std::string pull_policy = "IfNotPresent";
    std::string packager_secret = "live-packager-secrets";
    std::string k8s_api_url = "https://kubernetes.default.svc";
    std::string k8s_namespace;
    std::string k8s_token_file = "/var/run/secrets/kubernetes.io/serviceaccount/token";
    std::string k8s_ca_file = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
    // ULW_LIVE_BROADCASTER_CLAIM, "<claim>=<value>": who may start a stream. Empty: anyone
    // signed in.
    std::string broadcaster_claim;
    std::string broadcaster_value;
    LiveSettings settings;
    // LiveKit's webhooks (ADR-0093): the port of their own listener, 0 for none, and how the
    // publisher's comings and goings are followed.
    std::uint16_t webhook_port = 0;
    WatchSettings watch;
};

struct Config {
    std::uint16_t port = 8080;
    net::ReactorKind reactor = net::ReactorKind::IoUring;
    Transport transport = Transport::Plain;
    // ULW_JWKS_MAX_STALE_HOURS: how long keys stay trusted while every refetch fails.
    std::uint32_t jwks_max_stale_hours = 24;
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
    // Exactly one of the two: the identity provider's JWKS, or a local key set for offline
    // development.
    std::string jwks_url;
    std::string dev_jwks_file;
    // Its contents, read and checked by load_config.
    std::string dev_jwks;
    std::string jwt_issuer;
    // Required with jwks_url; ops::kDevAudience by default with a local key set.
    std::string jwt_audience;
    // ULW_JWT_SUBJECT_CLAIM: the claim that names the user, `sub` by default.
    std::string jwt_subject_claim = "sub";
    // ULW_SERVICE_CLAIM and ULW_SERVICE_SCOPE: which tokens are the operator's backend, for the
    // service API (ADR-0097). No value: no token is, and that API answers 403 to all.
    std::string service_claim = "scope";
    std::string service_value;
    // ULW_SERVICE_CLIENT_ID: with it, only tokens issued to that client (azp, or client_id).
    std::string service_client_id;
    // ULW_UPLOADER_CLAIM and ULW_UPLOADER_SCOPE (ADR-0100): only tokens whose claim holds the
    // value may create an upload. No value: every signed-in user may, as before.
    std::string uploader_claim = "scope";
    std::string uploader_value;
    // The size every chunk but an upload's last has, and the object store's part size.
    std::uint64_t chunk_size = std::uint64_t{8} << 20U;
    ops::Level log_level = ops::Level::Info;
    // Who to become when started as root.
    std::string run_as_user;
    // Stay root when started as root with no run_as_user; otherwise that is refused.
    bool allow_root = false;
    // ULW_DEV_MODE=1: a development run, which dev_jwks_file needs.
    bool dev_mode = false;
    Limits limits;
    LiveConfig live;
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
