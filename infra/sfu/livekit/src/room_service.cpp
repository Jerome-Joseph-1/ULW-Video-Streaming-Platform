#include "room_service.hpp"

#include "core/util/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace infra::sfu::livekit::detail {

namespace {

using core::ports::MediaError;

// RoomService answers from memory on a single node, in milliseconds; 5 s is far past any
// healthy answer and still short enough that a caller holding a user's join request can report
// the failure while the user is waiting.
constexpr core::Millis kRequestTimeout{5000};
// RoomService answers with the room or an empty object; a room with its codec list is under
// 1 KiB. Error bodies have curl's own bound.
constexpr std::size_t kMaxResponse = std::size_t{16} * 1024;
constexpr CallLimits kRoomServiceLimits{.timeout = kRequestTimeout, .max_response = kMaxResponse};

constexpr int kHttpNotFound = 404;
constexpr int kHttpRequestTimeout = 408;
constexpr int kHttpTooManyRequests = 429;

// Twirp answers 404 for a missing room or participant with the code not_found, but also for a
// method it does not know (bad_route), and a proxy in front answers 404 in its own words. Only
// the first means the thing is gone.
bool names_not_found(std::string_view body) noexcept {
    const auto error = core::json::parse(body);
    const core::json::Value* code = error ? error->find("code") : nullptr;
    return code != nullptr && code->as_string() == "not_found";
}

} // namespace

std::expected<void, MediaError> classify(const curl::Result& result, IfAbsent absent) noexcept {
    if (!result) {
        switch (result.error().kind) {
        case curl::FailureKind::Tls:
        case curl::FailureKind::Local:
            return std::unexpected(MediaError::Refused);
        case curl::FailureKind::Resolve:
        case curl::FailureKind::Connect:
        case curl::FailureKind::Timeout:
        case curl::FailureKind::Network:
        case curl::FailureKind::BodyTooLarge:
            return std::unexpected(MediaError::Unavailable);
        }
        return std::unexpected(MediaError::Unavailable);
    }
    const int status = result->status;
    if (status >= 200 && status < 300) {
        return {};
    }
    if (status == kHttpNotFound && absent == IfAbsent::Succeed && names_not_found(result->body)) {
        return {};
    }
    // Twirp's deadline_exceeded and canceled are 408, resource_exhausted is 429, and internal,
    // unavailable and unknown are 5xx.
    if (status == kHttpRequestTimeout || status == kHttpTooManyRequests || status >= 500) {
        return std::unexpected(MediaError::Unavailable);
    }
    return std::unexpected(MediaError::Refused);
}

class RoomService::Call final : public curl::ITransferHandler,
                                public curl::IBodySource,
                                public net::ITimerHandler {
public:
    Call(RoomService& owner, std::string body, IfAbsent absent, AnswerDone done) noexcept
        : owner_(owner), body_(std::move(body)), absent_(absent), done_(std::move(done)) {}

    ~Call() override {
        if (timer_) {
            owner_.reactor_.cancel_timer(*timer_);
        }
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    [[nodiscard]] std::uint64_t body_size() const noexcept { return body_.size(); }

    void start(std::unique_ptr<curl::Transfer> transfer) noexcept {
        transfer_ = std::move(transfer);
    }

    // For a call that failed before reaching the network: the port promises the callback never
    // runs inside the call that was given it.
    void fail_later(MediaError error) {
        failure_ = error;
        timer_ = owner_.reactor_.arm_timer(core::Millis{0}, *this);
    }

    void on_transfer_done(curl::Result result) noexcept override {
        const auto outcome = classify(result, absent_);
        if (!outcome) {
            owner_.finished(*this, std::unexpected(outcome.error()));
            return;
        }
        owner_.finished(*this, result ? std::move(result->body) : std::string{});
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

    [[nodiscard]] AnswerDone take_callback() noexcept { return std::move(done_); }

private:
    RoomService& owner_;
    std::string body_;
    std::size_t sent_ = 0;
    IfAbsent absent_;
    AnswerDone done_;
    std::optional<net::TimerId> timer_;
    MediaError failure_ = MediaError::Unavailable;
    std::unique_ptr<curl::Transfer> transfer_;
};

RoomService::RoomService(net::IReactor& reactor, curl::Multi& multi,
                         const core::ports::IClock& clock, std::string api_url, ApiKey key) noexcept
    : reactor_(reactor), multi_(multi), clock_(clock), api_url_(std::move(api_url)),
      key_(std::move(key)) {}

RoomService::~RoomService() = default;

void RoomService::call(std::string_view method, std::string body, const Grant& grant,
                       IfAbsent absent, core::ports::MediaDone done) {
    start(method, std::move(body), grant, absent, kRoomServiceLimits,
          [done = std::move(done)](Answer answer) mutable noexcept {
              if (!answer) {
                  done(std::unexpected(answer.error()));
                  return;
              }
              done({});
          });
}

void RoomService::fetch(std::string_view method, std::string body, const Grant& grant,
                        const CallLimits& limits, AnswerDone done) {
    start(method, std::move(body), grant, IfAbsent::Fail, limits, std::move(done));
}

void RoomService::start(std::string_view method, std::string body, const Grant& grant,
                        IfAbsent absent, const CallLimits& limits, AnswerDone done) {
    auto& call = *calls_.emplace_back(
        std::make_unique<Call>(*this, std::move(body), absent, std::move(done)));
    // Twice the request it authorises; LiveKit also allows a minute of clock skew.
    const auto token = mint_token(key_, grant, clock_.wall_now(),
                                  2 * std::chrono::ceil<core::Seconds>(limits.timeout));
    if (!token) {
        call.fail_later(MediaError::Refused);
        return;
    }
    std::string url = api_url_;
    url += "/twirp/livekit.";
    url += method;
    auto transfer = curl::Transfer::start_upload(
        multi_,
        curl::Request{
            .method = curl::Method::Post,
            .url = std::move(url),
            .headers = {"Content-Type: application/json", "Authorization: Bearer " + token->jwt},
            .max_body = limits.max_response,
            .timeout = limits.timeout},
        call.body_size(), call, call);
    if (!transfer) {
        call.fail_later(classify(std::unexpected(std::move(transfer.error())), absent).error());
        return;
    }
    call.start(std::move(*transfer));
}

void RoomService::fail(core::ports::MediaError error, AnswerDone done) {
    calls_
        .emplace_back(std::make_unique<Call>(*this, std::string{}, IfAbsent::Fail, std::move(done)))
        ->fail_later(error);
}

void RoomService::finished(Call& call, Answer outcome) noexcept {
    AnswerDone done = call.take_callback();
    // Destroys the call and its transfer, which libcurl has already let go of.
    std::erase_if(calls_, [&](const auto& c) { return c.get() == &call; });
    // Last: the callback may destroy this service.
    done(std::move(outcome));
}

} // namespace infra::sfu::livekit::detail
