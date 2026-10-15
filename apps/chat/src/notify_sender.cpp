#include "notify_sender.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <new>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <span>
#include <utility>

namespace chat {

namespace {

constexpr int kHttpOkFirst = 200;
constexpr int kHttpOkLast = 299;
// The receiver's answer is read only for its status.
constexpr std::size_t kMaxResponse = 4'096;

} // namespace

std::string notify_signature(std::string_view secret, std::int64_t unix_seconds,
                             std::string_view body) {
    std::string signed_text = std::to_string(unix_seconds);
    signed_text += '.';
    signed_text += body;
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_size = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* data = reinterpret_cast<const unsigned char*>(signed_text.data());
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), data, signed_text.size(),
         mac.data(), &mac_size);
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out = "t=" + std::to_string(unix_seconds) + ",v1=";
    for (unsigned int i = 0; i < mac_size; ++i) {
        out += kHex[mac.at(i) >> 4U];
        out += kHex[mac.at(i) & 0x0FU];
    }
    return out;
}

class HookSender::Request final : public infra::curl::ITransferHandler,
                                  public infra::curl::IBodySource,
                                  public net::ITimerHandler {
public:
    Request(HookSender& owner, std::string body, std::size_t events) noexcept
        : owner_(owner), body_(std::move(body)), events_(events) {}
    ~Request() override {
        if (timer_) {
            owner_.reactor_.cancel_timer(*timer_);
        }
    }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    [[nodiscard]] std::size_t events() const noexcept { return events_; }
    [[nodiscard]] const std::string& body() const noexcept { return body_; }

    void start(std::unique_ptr<infra::curl::Transfer> transfer) noexcept {
        transfer_ = std::move(transfer);
    }
    // For a request that failed before reaching the network: answered on a later turn, never
    // from inside offer().
    void fail_later() { timer_ = owner_.reactor_.arm_timer(core::Millis{0}, *this); }

    void on_transfer_done(infra::curl::Result result) noexcept override {
        owner_.finished(result && result->status >= kHttpOkFirst && result->status <= kHttpOkLast);
    }
    void on_timeout() noexcept override {
        timer_.reset();
        owner_.finished(false);
    }
    std::size_t read_body(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min(out.size(), body_.size() - sent_);
        std::memcpy(out.data(), body_.data() + sent_, n);
        sent_ += n;
        return n;
    }

private:
    HookSender& owner_;
    std::string body_;
    std::size_t events_;
    std::size_t sent_ = 0;
    std::optional<net::TimerId> timer_;
    std::unique_ptr<infra::curl::Transfer> transfer_;
};

HookSender::HookSender(net::IReactor& reactor, infra::curl::Multi& multi,
                       const core::ports::IClock& clock, std::string url, std::string secret,
                       SenderLimits limits)
    : reactor_(reactor), multi_(multi), clock_(clock), url_(std::move(url)),
      secret_(std::move(secret)), limits_(limits) {
    limits_.batch = std::max<std::size_t>(limits_.batch, 1);
}

HookSender::~HookSender() = default;

void HookSender::offer(std::string event) noexcept {
    if (queue_.size() >= limits_.max_queued) {
        ++counters_.dropped;
        return;
    }
    try {
        queue_.push_back(std::move(event));
    } catch (const std::bad_alloc&) {
        ++counters_.dropped;
        return;
    }
    if (request_ == nullptr) {
        start();
    }
}

void HookSender::start() noexcept {
    if (queue_.empty()) {
        return;
    }
    const std::size_t n = std::min(queue_.size(), limits_.batch);
    try {
        std::string body = R"({"events":[)";
        for (std::size_t i = 0; i < n; ++i) {
            if (i != 0) {
                body += ',';
            }
            body += queue_[i];
        }
        body += "]}";
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(clock_.wall_now().time_since_epoch())
                .count();
        std::string signature = notify_signature(secret_, seconds, body);
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
        request_ = std::make_unique<Request>(*this, std::move(body), n);
        auto transfer = infra::curl::Transfer::start_upload(
            multi_,
            infra::curl::Request{
                .method = infra::curl::Method::Post,
                .url = url_,
                .headers = {"Content-Type: application/json", "ULW-Signature: " + signature},
                .max_body = kMaxResponse,
                .timeout = std::chrono::milliseconds(limits_.timeout.count())},
            request_->body().size(), *request_, *request_);
        if (!transfer) {
            request_->fail_later();
            return;
        }
        request_->start(std::move(*transfer));
    } catch (const std::bad_alloc&) {
        // The batch is lost; the next offer tries again.
        counters_.dropped += request_ == nullptr ? n : 0;
        if (request_ == nullptr) {
            queue_.erase(queue_.begin(),
                         queue_.begin() + static_cast<std::ptrdiff_t>(std::min(n, queue_.size())));
        }
    }
}

void HookSender::finished(bool ok) noexcept {
    const std::size_t events = request_->events();
    ++counters_.requests;
    if (ok) {
        counters_.sent += events;
    } else {
        counters_.failed += events;
        ++counters_.failed_requests;
    }
    // Destroys the request and its transfer, which libcurl has already let go of.
    request_.reset();
    start();
}

} // namespace chat
