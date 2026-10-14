#include "cloudflare.hpp"

#include "core/util/json.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace infra::turn {

namespace {

using core::ports::IceServer;
using core::ports::MediaError;

// The API answers in well under a second; 5 s still leaves a joining user waiting for a ticket
// a bounded time, as a RoomService call does.
constexpr std::chrono::milliseconds kTimeout{5000};
// A STUN entry and a TURN entry with eight URIs and a credential pair: under 1 KiB.
constexpr std::size_t kMaxResponse = std::size_t{16} * 1024;

constexpr int kHttpRequestTimeout = 408;
constexpr int kHttpTooManyRequests = 429;

// The port of a STUN or TURN URI, "turn:host:53?transport=udp" -> "53".
std::string_view port_of(std::string_view url) noexcept {
    url = url.substr(0, url.find('?'));
    const std::size_t colon = url.rfind(':');
    // An IPv6 host's brackets end before its port.
    if (colon == std::string_view::npos || url.find(']', colon) != std::string_view::npos) {
        return {};
    }
    return url.substr(colon + 1);
}

std::optional<IceServer> server_of(const core::json::Value& entry) {
    const core::json::Value* urls = entry.find("urls");
    if (urls == nullptr) {
        return std::nullopt;
    }
    IceServer server;
    const auto add = [&](const core::json::Value& value) {
        const auto url = value.as_string();
        if (!url) {
            return false;
        }
        if (port_of(*url) != "53") {
            server.urls.emplace_back(*url);
        }
        return true;
    };
    if (const auto* list = urls->as_array()) {
        if (!std::ranges::all_of(*list, add)) {
            return std::nullopt;
        }
    } else if (!add(*urls)) {
        return std::nullopt;
    }
    for (const auto& [key, into] :
         {std::pair{"username", &server.username}, std::pair{"credential", &server.credential}}) {
        if (const core::json::Value* value = entry.find(key)) {
            const auto text = value->as_string();
            if (!text) {
                return std::nullopt;
            }
            *into = *text;
        }
    }
    return server;
}

std::expected<void, MediaError> classify(const curl::Result& result) noexcept {
    if (!result) {
        const curl::FailureKind kind = result.error().kind;
        // A certificate refused or a request that could not be made will fail again.
        if (kind == curl::FailureKind::Tls || kind == curl::FailureKind::Local) {
            return std::unexpected(MediaError::Refused);
        }
        return std::unexpected(MediaError::Unavailable);
    }
    const int status = result->status;
    if (status >= 200 && status < 300) {
        return {};
    }
    if (status == kHttpRequestTimeout || status == kHttpTooManyRequests || status >= 500) {
        return std::unexpected(MediaError::Unavailable);
    }
    // 401 and 403: the key's token is wrong or revoked; 400 and 404: the key id is.
    return std::unexpected(MediaError::Refused);
}

} // namespace

namespace detail {

std::optional<std::vector<IceServer>> parse_cloudflare(std::string_view body) {
    const auto answer = core::json::parse(body);
    const core::json::Value* servers = answer ? answer->find("iceServers") : nullptr;
    if (servers == nullptr) {
        return std::nullopt;
    }
    std::vector<IceServer> out;
    // An array of servers; the API's first version answered a single object.
    if (const auto* list = servers->as_array()) {
        for (const core::json::Value& entry : *list) {
            auto server = server_of(entry);
            if (!server) {
                return std::nullopt;
            }
            if (!server->urls.empty()) {
                out.push_back(std::move(*server));
            }
        }
    } else if (auto server = server_of(*servers)) {
        if (!server->urls.empty()) {
            out.push_back(std::move(*server));
        }
    } else {
        return std::nullopt;
    }
    const bool relays = std::ranges::any_of(out, [](const IceServer& s) {
        return !s.username.empty() && std::ranges::any_of(s.urls, [](const std::string& url) {
            return url.starts_with("turn");
        });
    });
    if (!relays) {
        return std::nullopt;
    }
    return out;
}

} // namespace detail

class CloudflareCredentials::Request final : public curl::ITransferHandler,
                                             public curl::IBodySource,
                                             public net::ITimerHandler {
public:
    Request(CloudflareCredentials& owner, std::string body,
            core::ports::IceServersDone done) noexcept
        : owner_(owner), body_(std::move(body)), done_(std::move(done)) {}

    ~Request() override {
        if (timer_) {
            owner_.reactor_.cancel_timer(*timer_);
        }
    }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    [[nodiscard]] std::uint64_t body_size() const noexcept { return body_.size(); }

    void start(std::unique_ptr<curl::Transfer> transfer) noexcept {
        transfer_ = std::move(transfer);
    }

    // For a request that failed before reaching the network: never from inside issue.
    void fail_later(MediaError error) {
        failure_ = error;
        timer_ = owner_.reactor_.arm_timer(core::Millis{0}, *this);
    }

    void on_transfer_done(curl::Result result) noexcept override {
        if (const auto outcome = classify(result); !outcome) {
            owner_.finished(*this, std::unexpected(outcome.error()));
            return;
        }
        auto servers = detail::parse_cloudflare(result->body);
        if (!servers) {
            // A 2xx we cannot read: the API changed under us or a proxy answered.
            owner_.finished(*this, std::unexpected(MediaError::Unavailable));
            return;
        }
        owner_.finished(*this, std::move(*servers));
    }

    void on_timeout() noexcept override {
        timer_.reset();
        owner_.finished(*this, std::unexpected(failure_));
    }

    std::size_t read_body(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min(out.size(), body_.size() - sent_);
        std::memcpy(out.data(), body_.data() + sent_, n);
        sent_ += n;
        return n;
    }

    [[nodiscard]] core::ports::IceServersDone take_callback() noexcept { return std::move(done_); }

private:
    CloudflareCredentials& owner_;
    std::string body_;
    std::size_t sent_ = 0;
    core::ports::IceServersDone done_;
    std::optional<net::TimerId> timer_;
    MediaError failure_ = MediaError::Unavailable;
    std::unique_ptr<curl::Transfer> transfer_;
};

CloudflareCredentials::CloudflareCredentials(net::IReactor& reactor, curl::Multi& multi,
                                             Config config) noexcept
    : reactor_(reactor), multi_(multi), config_(std::move(config)),
      url_(config_.api_url + "/v1/turn/keys/" + config_.key_id +
           "/credentials/generate-ice-servers") {}

CloudflareCredentials::~CloudflareCredentials() = default;

void CloudflareCredentials::issue(const core::UserId& /*user*/, const core::DeviceId& /*device*/,
                                  core::ports::IceServersDone done) {
    auto& request = *requests_.emplace_back(std::make_unique<Request>(
        *this, R"({"ttl":)" + std::to_string(config_.ttl.count()) + "}", std::move(done)));
    auto transfer = curl::Transfer::start_upload(
        multi_,
        curl::Request{.method = curl::Method::Post,
                      .url = url_,
                      .headers = {"Content-Type: application/json",
                                  "Authorization: Bearer " + config_.api_token},
                      .max_body = kMaxResponse,
                      .timeout = kTimeout},
        request.body_size(), request, request);
    if (!transfer) {
        request.fail_later(classify(std::unexpected(std::move(transfer.error()))).error());
        return;
    }
    request.start(std::move(*transfer));
}

void CloudflareCredentials::finished(
    Request& request,
    std::expected<std::vector<core::ports::IceServer>, core::ports::MediaError> outcome) noexcept {
    auto done = request.take_callback();
    // Destroys the request and its transfer, which libcurl has already let go of.
    std::erase_if(requests_, [&](const auto& r) { return r.get() == &request; });
    // Last: the callback may destroy this issuer.
    done(std::move(outcome));
}

} // namespace infra::turn
