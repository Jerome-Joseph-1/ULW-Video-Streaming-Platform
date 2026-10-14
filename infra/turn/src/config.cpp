#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "core/util/url.hpp"
#include "infra/turn/turn.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace infra::turn {

namespace {

// Ten minutes: shorter and a slow joiner's credential could lapse before its first allocation.
// Two days: longer, and a credential that leaked keeps working well past any call.
constexpr std::uint32_t kMinTtl = 600;
constexpr std::uint32_t kMaxTtl = 172'800;
constexpr std::size_t kMaxUrls = 16;

std::optional<std::string> lookup(const EnvLookup& env, std::string_view name) {
    auto value = env(name);
    if (value && value->empty()) {
        return std::nullopt;
    }
    return value;
}

std::unexpected<ConfigError> error(std::string_view variable, std::string_view reason) {
    return std::unexpected(
        ConfigError{.variable = std::string(variable), .reason = std::string(reason)});
}

std::expected<std::string, ConfigError> required(const EnvLookup& env, std::string_view name,
                                                 std::string_view provider) {
    auto value = lookup(env, name);
    if (!value) {
        return error(name, "required with ULW_TURN_PROVIDER=" + std::string(provider));
    }
    return std::move(*value);
}

// A STUN or TURN URI as RFC 7064 and RFC 7065 write them, which is what a browser takes.
bool ice_url(std::string_view url) {
    const bool scheme = url.starts_with("turn:") || url.starts_with("turns:") ||
                        url.starts_with("stun:") || url.starts_with("stuns:");
    return scheme && url.find(':') + 1 < url.size() &&
           std::ranges::none_of(url, [](char c) { return c == ' ' || c == ',' || c == '"'; });
}

std::expected<std::vector<std::string>, ConfigError> urls(const EnvLookup& env) {
    auto text = required(env, "ULW_TURN_URLS", "hmac");
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    std::vector<std::string> out;
    std::string_view rest = *text;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view url = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (!ice_url(url)) {
            return error("ULW_TURN_URLS", "expected comma-separated turn:, turns:, stun: or "
                                          "stuns: URIs");
        }
        out.emplace_back(url);
    }
    if (out.size() > kMaxUrls) {
        return error("ULW_TURN_URLS", "more than 16 URIs");
    }
    const bool relays =
        std::ranges::any_of(out, [](const std::string& url) { return url.starts_with("turn"); });
    if (!relays) {
        return error("ULW_TURN_URLS", "names no turn: or turns: URI");
    }
    return out;
}

std::expected<void, ConfigError> load_hmac(const EnvLookup& env, Config& config) {
    for (const std::string_view name :
         {"ULW_TURN_KEY_ID", "ULW_TURN_KEY_API_TOKEN", "ULW_TURN_API_URL"}) {
        if (lookup(env, name)) {
            return error(name, "set, but ULW_TURN_PROVIDER is hmac");
        }
    }
    auto list = urls(env);
    if (!list) {
        return std::unexpected(std::move(list.error()));
    }
    auto secret = required(env, "ULW_TURN_SHARED_SECRET", "hmac");
    if (!secret) {
        return std::unexpected(std::move(secret.error()));
    }
    // A secret short enough to guess lets anyone mint credentials for the relay.
    constexpr std::size_t kMinSecret = 16;
    if (secret->size() < kMinSecret) {
        return error("ULW_TURN_SHARED_SECRET", "shorter than 16 bytes");
    }
    config.provider = Provider::Hmac;
    config.urls = std::move(*list);
    config.shared_secret = std::move(*secret);
    return {};
}

std::expected<void, ConfigError> load_cloudflare(const EnvLookup& env, Config& config) {
    for (const std::string_view name : {"ULW_TURN_URLS", "ULW_TURN_SHARED_SECRET"}) {
        if (lookup(env, name)) {
            return error(name, "set, but ULW_TURN_PROVIDER is cloudflare");
        }
    }
    auto key = required(env, "ULW_TURN_KEY_ID", "cloudflare");
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    // It goes into the API's path.
    const bool path_safe = std::ranges::all_of(*key, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '-' || c == '_';
    });
    if (!path_safe) {
        return error("ULW_TURN_KEY_ID", "expected letters, digits, '-' and '_' only");
    }
    auto token = required(env, "ULW_TURN_KEY_API_TOKEN", "cloudflare");
    if (!token) {
        return std::unexpected(std::move(token.error()));
    }
    if (auto api = lookup(env, "ULW_TURN_API_URL")) {
        // The token rides in every request: https, or http to this machine for a test.
        if (!core::secure_url(*api, "https", "http", core::loopback_host) || api->ends_with('/')) {
            return error("ULW_TURN_API_URL",
                         "must be an https URL without a trailing / (http only to loopback)");
        }
        config.api_url = std::move(*api);
    }
    config.provider = Provider::Cloudflare;
    config.key_id = std::move(*key);
    config.api_token = std::move(*token);
    return {};
}

} // namespace

void append_ice_servers(std::string& out, std::span<const core::ports::IceServer> servers) {
    if (servers.empty()) {
        return;
    }
    out += R"("ice_servers":[)";
    for (std::size_t i = 0; i < servers.size(); ++i) {
        const core::ports::IceServer& server = servers[i];
        out += i == 0 ? R"({"urls":[)" : R"(,{"urls":[)";
        for (std::size_t u = 0; u < server.urls.size(); ++u) {
            if (u != 0) {
                out += ',';
            }
            core::json::append_string(out, server.urls[u]);
        }
        out += ']';
        if (!server.username.empty()) {
            out += R"(,"username":)";
            core::json::append_string(out, server.username);
            out += R"(,"credential":)";
            core::json::append_string(out, server.credential);
        }
        out += '}';
    }
    out += ']';
}

std::string_view describe(const std::optional<Config>& config) noexcept {
    if (!config) {
        return "self-hosted";
    }
    return config->provider == Provider::Cloudflare ? "external cloudflare" : "external hmac";
}

std::expected<std::optional<Config>, ConfigError> load_config(const EnvLookup& env) {
    const std::string mode = lookup(env, "ULW_TURN_MODE").value_or("self-hosted");
    if (mode == "self-hosted") {
        for (const std::string_view name : kVariables) {
            if (name != "ULW_TURN_MODE" && lookup(env, name)) {
                return error(name, "set, but ULW_TURN_MODE is not external");
            }
        }
        return std::nullopt;
    }
    if (mode != "external") {
        return error("ULW_TURN_MODE", "expected self-hosted or external");
    }
    Config config;
    const std::string provider = lookup(env, "ULW_TURN_PROVIDER").value_or("");
    if (provider == "hmac") {
        if (auto r = load_hmac(env, config); !r) {
            return std::unexpected(std::move(r.error()));
        }
    } else if (provider == "cloudflare") {
        if (auto r = load_cloudflare(env, config); !r) {
            return std::unexpected(std::move(r.error()));
        }
    } else {
        return error("ULW_TURN_PROVIDER", "expected hmac or cloudflare");
    }
    if (auto ttl = lookup(env, "ULW_TURN_TTL_SECONDS")) {
        const auto seconds = core::parse_integer<std::uint32_t>(*ttl);
        if (!seconds || *seconds < kMinTtl || *seconds > kMaxTtl) {
            return error("ULW_TURN_TTL_SECONDS", "expected 600 to 172800");
        }
        config.ttl = core::Seconds{*seconds};
    }
    return config;
}

} // namespace infra::turn
