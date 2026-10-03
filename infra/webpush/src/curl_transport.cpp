#include "infra/webpush/curl_transport.hpp"

#include "core/util/parse.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <span>
#include <utility>

namespace infra::webpush {

namespace {

// A push service's answer is a few hundred bytes; anything past this is not kept.
constexpr std::size_t kMaxResponseBody = 4096;
// A Retry-After longer than a day says "not today"; the message's deadline is far shorter.
constexpr std::uint32_t kMaxRetryAfter = 86'400;

} // namespace

class CurlPushTransport::Post final : public curl::ITransferHandler, public curl::IBodySource {
public:
    Post(CurlPushTransport& owner, std::uint64_t id, std::vector<std::uint8_t> body,
         Done done) noexcept
        : owner_(owner), id_(id), body_(std::move(body)), done_(std::move(done)) {}

    [[nodiscard]] bool start(const PushRequest& request) {
        const curl::Request req{.method = curl::Method::Post,
                                .url = request.url,
                                .headers = request.headers,
                                .max_body = kMaxResponseBody,
                                .timeout = owner_.options_.timeout,
                                .public_only = owner_.options_.public_only,
                                .https_only = true,
                                .ca_file = owner_.options_.ca_file};
        auto transfer =
            curl::Transfer::start_upload(owner_.multi_, req, body_.size(), *this, *this);
        if (!transfer) {
            return false;
        }
        transfer_ = std::move(*transfer);
        return true;
    }

    std::size_t read_body(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min(out.size(), body_.size() - sent_);
        std::memcpy(out.data(), body_.data() + sent_, n);
        sent_ += n;
        return n;
    }

    void on_transfer_done(curl::Result result) noexcept override {
        Done done = std::move(done_);
        const PushOutcome outcome = outcome_of(result);
        // Destroys this.
        owner_.finished(id_);
        done(outcome);
    }

private:
    CurlPushTransport& owner_;
    std::uint64_t id_;
    std::vector<std::uint8_t> body_;
    std::size_t sent_ = 0;
    Done done_;
    std::unique_ptr<curl::Transfer> transfer_;
};

PushOutcome outcome_of(const curl::Result& result) noexcept {
    if (!result) {
        switch (result.error().kind) {
        case curl::FailureKind::AddressRefused:
            return std::unexpected(PushFailure::AddressRefused);
        case curl::FailureKind::Tls:
            return std::unexpected(PushFailure::Tls);
        case curl::FailureKind::Local:
            return std::unexpected(PushFailure::Local);
        case curl::FailureKind::Resolve:
        case curl::FailureKind::Connect:
        case curl::FailureKind::Timeout:
        case curl::FailureKind::Network:
        case curl::FailureKind::BodyTooLarge:
            return std::unexpected(PushFailure::Network);
        }
        return std::unexpected(PushFailure::Network);
    }
    PushResponse response{.status = result->status, .retry_after = std::nullopt};
    // Delta-seconds only; an HTTP-date is left to the sender's own backoff.
    if (const auto value = result->header("retry-after")) {
        if (const auto seconds = core::parse_integer<std::uint32_t>(*value);
            seconds && *seconds <= kMaxRetryAfter) {
            response.retry_after = core::Seconds{*seconds};
        }
    }
    return response;
}

CurlPushTransport::CurlPushTransport(curl::Multi& multi, CurlTransportOptions options) noexcept
    : multi_(multi), options_(std::move(options)) {}

CurlPushTransport::~CurlPushTransport() = default;

bool CurlPushTransport::post(PushRequest request, Done done) noexcept {
    try {
        const std::uint64_t id = next_++;
        auto post = std::make_unique<Post>(*this, id, std::move(request.body), std::move(done));
        Post& p = *post;
        posts_.emplace(id, std::move(post));
        if (!p.start(request)) {
            posts_.erase(id);
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void CurlPushTransport::finished(std::uint64_t id) noexcept {
    posts_.erase(id);
}

} // namespace infra::webpush
