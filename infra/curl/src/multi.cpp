#include "infra/curl/multi.hpp"

#include "exchange.hpp"

#include <algorithm>
#include <cstdint>
#include <curl/curl.h>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace infra::curl {

class Multi::Impl final : public net::ITimerHandler {
public:
    Impl(net::IReactor& reactor, CURLM* handle) noexcept : reactor_(reactor), handle_(handle) {}
    ~Impl() override;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool configure(std::size_t max_connections) noexcept;
    [[nodiscard]] std::expected<void, Failure> attach(Transfer& transfer);
    // Removes the transfer from libcurl. False if it was not attached (already completed).
    bool release(Transfer& transfer) noexcept;

    void on_timeout() noexcept override;

private:
    class Watch final : public net::IReadyHandler {
    public:
        explicit Watch(Impl& owner) noexcept : owner_(owner) {}
        void on_ready(net::Interest ready) noexcept override;

        [[nodiscard]] curl_socket_t fd() const noexcept { return fd_; }
        void assign(curl_socket_t fd) noexcept { fd_ = fd; }

    private:
        Impl& owner_;
        curl_socket_t fd_ = CURL_SOCKET_BAD;
    };

    struct MultiCleanup {
        void operator()(CURLM* handle) const noexcept { curl_multi_cleanup(handle); }
    };

    static int on_socket(CURL* easy, curl_socket_t fd, int what, void* self, void* socket) noexcept;
    static int on_timer(CURLM* handle, long timeout_ms, void* self) noexcept;
    [[nodiscard]] static Transfer* transfer_of(CURL* easy) noexcept;

    void act(curl_socket_t fd, int events) noexcept;
    void complete_finished() noexcept;
    void fail_all(const char* why) noexcept;
    void arm(core::Millis delay) noexcept;
    void disarm() noexcept;
    Watch& acquire(curl_socket_t fd);
    void doom(CURL* easy) noexcept;
    static void complete(Transfer& transfer, Result result) noexcept;

    net::IReactor& reactor_;
    std::unique_ptr<CURLM, MultiCleanup> handle_;
    net::TimerId timer_{};
    std::vector<Transfer*> attached_;
    // Still attached, but their socket was refused by the reactor; they fail on the next pass.
    std::vector<Transfer*> doomed_;
    // Already detached by fail_all, handlers not yet called.
    std::vector<Transfer*> failing_;
    // Watches are recycled, never freed while the multi lives: libcurl may drop a socket in
    // the middle of the socket_action call that the socket's own on_ready is making.
    std::vector<std::unique_ptr<Watch>> watches_;
    std::vector<Watch*> idle_;
};

Multi::Impl::~Impl() {
    disarm();
    for (const auto& w : watches_) {
        if (w->fd() != CURL_SOCKET_BAD) {
            reactor_.unwatch(w->fd());
        }
    }
    // Cached connections are closed by curl_multi_cleanup; with the callbacks unset it does
    // not report them to a reactor that has already let go of them.
    static_cast<void>(curl_multi_setopt(handle_.get(), CURLMOPT_SOCKETFUNCTION,
                                        static_cast<curl_socket_callback>(nullptr)));
    static_cast<void>(curl_multi_setopt(handle_.get(), CURLMOPT_TIMERFUNCTION,
                                        static_cast<curl_multi_timer_callback>(nullptr)));
}

bool Multi::Impl::configure(std::size_t max_connections) noexcept {
    CURLM* const m = handle_.get();
    // Nothing in libcurl limits the wait of a transfer queued behind the connection cap: it
    // has no connection, and 8.5 checks neither the connect timeout nor CURLOPT_TIMEOUT
    // without one. So whatever shares a multi with uploads must be able to wait behind them;
    // the gateway gives its key fetches a multi of their own, and a part stuck in the queue
    // takes no bytes, which the gateway's body timeout ends.
    const auto cap = static_cast<long>(max_connections);
    return curl_multi_setopt(m, CURLMOPT_SOCKETFUNCTION, &Impl::on_socket) == CURLM_OK &&
           curl_multi_setopt(m, CURLMOPT_SOCKETDATA, this) == CURLM_OK &&
           curl_multi_setopt(m, CURLMOPT_TIMERFUNCTION, &Impl::on_timer) == CURLM_OK &&
           curl_multi_setopt(m, CURLMOPT_TIMERDATA, this) == CURLM_OK &&
           curl_multi_setopt(m, CURLMOPT_MAX_TOTAL_CONNECTIONS, cap) == CURLM_OK;
}

std::expected<void, Failure> Multi::Impl::attach(Transfer& transfer) {
    CURL* const easy = transfer.exchange_->easy();
    if (curl_easy_setopt(easy, CURLOPT_PRIVATE, static_cast<void*>(&transfer)) != CURLE_OK) {
        return std::unexpected(
            Failure{.kind = FailureKind::Local, .detail = "curl option refused"});
    }
    attached_.push_back(&transfer);
    // Starts nothing by itself: libcurl asks for a zero timeout, which fires on the next
    // loop iteration.
    if (const CURLMcode rc = curl_multi_add_handle(handle_.get(), easy); rc != CURLM_OK) {
        attached_.pop_back();
        return std::unexpected(
            Failure{.kind = FailureKind::Local, .detail = curl_multi_strerror(rc)});
    }
    return {};
}

bool Multi::Impl::release(Transfer& transfer) noexcept {
    std::erase(doomed_, &transfer);
    std::erase(failing_, &transfer);
    const auto it = std::ranges::find(attached_, &transfer);
    if (it == attached_.end()) {
        return false;
    }
    *it = attached_.back();
    attached_.pop_back();
    // Before curl_easy_cleanup, which the Exchange does later: cleaning up a handle still in
    // a multi leaves the multi pointing at freed memory.
    static_cast<void>(curl_multi_remove_handle(handle_.get(), transfer.exchange_->easy()));
    return true;
}

void Multi::Impl::on_timeout() noexcept {
    timer_ = {};
    act(CURL_SOCKET_TIMEOUT, 0);
    if (timer_ == net::TimerId{}) {
        // The reactor may fire a little before libcurl's own clock agrees the timeout is due.
        // libcurl then does nothing and, its deadline unchanged, never asks for the timer
        // again; without this the transfer would stall.
        long ms = -1;
        if (curl_multi_timeout(handle_.get(), &ms) == CURLM_OK && ms >= 0) {
            arm(core::Millis{ms});
        }
    }
}

void Multi::Impl::Watch::on_ready(net::Interest ready) noexcept {
    int events = 0;
    if (net::has(ready, net::Interest::Read)) {
        events |= CURL_CSELECT_IN;
    }
    if (net::has(ready, net::Interest::Write)) {
        events |= CURL_CSELECT_OUT;
    }
    // This watch may be recycled for another socket before act returns; nothing below reads
    // it again.
    owner_.act(fd_, events);
}

int Multi::Impl::on_socket(CURL* easy, curl_socket_t fd, int what, void* self,
                           void* socket) noexcept {
    auto& impl = *static_cast<Impl*>(self);
    auto* watch = static_cast<Watch*>(socket);
    if (what == CURL_POLL_REMOVE) {
        if (watch != nullptr) {
            impl.reactor_.unwatch(fd);
            watch->assign(CURL_SOCKET_BAD);
            impl.idle_.push_back(watch);
        }
        return 0;
    }
    if (watch == nullptr) {
        watch = &impl.acquire(fd);
        static_cast<void>(curl_multi_assign(impl.handle_.get(), fd, watch));
    }
    net::Interest interest = net::Interest::None;
    if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
        interest = interest | net::Interest::Read;
    }
    if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
        interest = interest | net::Interest::Write;
    }
    if (!impl.reactor_.watch(fd, interest, *watch)) {
        // Returning -1 would make libcurl abort every transfer on this multi; only the one
        // whose socket cannot be watched has to go.
        impl.doom(easy);
    }
    return 0;
}

int Multi::Impl::on_timer(CURLM* /*handle*/, long timeout_ms, void* self) noexcept {
    auto& impl = *static_cast<Impl*>(self);
    if (timeout_ms < 0) {
        impl.disarm();
    } else {
        // Never socket_action from here, least of all for 0: libcurl would recurse into this
        // callback. A zero delay fires on the next loop iteration instead.
        impl.arm(core::Millis{timeout_ms});
    }
    return 0;
}

Transfer* Multi::Impl::transfer_of(CURL* easy) noexcept {
    void* data = nullptr;
    if (easy == nullptr || curl_easy_getinfo(easy, CURLINFO_PRIVATE, &data) != CURLE_OK) {
        return nullptr;
    }
    return static_cast<Transfer*>(data);
}

void Multi::Impl::act(curl_socket_t fd, int events) noexcept {
    int running = 0;
    if (const CURLMcode rc = curl_multi_socket_action(handle_.get(), fd, events, &running);
        rc != CURLM_OK) {
        // libcurl documents every transfer's state as unknown after this.
        fail_all(curl_multi_strerror(rc));
        return;
    }
    complete_finished();
}

void Multi::Impl::complete_finished() noexcept {
    int queued = 0;
    // Read afresh after every handler: one may cancel another transfer, and removing a
    // handle also withdraws its queued message.
    while (CURLMsg* msg = curl_multi_info_read(handle_.get(), &queued)) {
        if (msg->msg != CURLMSG_DONE) {
            continue;
        }
        const CURLcode code = msg->data.result;
        Transfer* const transfer = transfer_of(msg->easy_handle);
        if (transfer == nullptr || !release(*transfer)) {
            continue;
        }
        complete(*transfer, transfer->exchange_->finish(code));
    }
    while (!doomed_.empty()) {
        Transfer* const transfer = doomed_.back();
        if (release(*transfer)) {
            complete(*transfer, std::unexpected(Failure{.kind = FailureKind::Local,
                                                        .detail = "socket not watchable"}));
        }
    }
}

void Multi::Impl::fail_all(const char* why) noexcept {
    // Every handle leaves before any handler runs: a handler may start a new transfer, and
    // libcurl only accepts new handles on a failed multi once all the old ones are gone.
    for (Transfer* const transfer : attached_) {
        static_cast<void>(curl_multi_remove_handle(handle_.get(), transfer->exchange_->easy()));
    }
    failing_.insert(failing_.end(), attached_.begin(), attached_.end());
    attached_.clear();
    doomed_.clear();
    while (!failing_.empty()) {
        Transfer* const transfer = failing_.back();
        failing_.pop_back();
        complete(*transfer,
                 std::unexpected(Failure{.kind = FailureKind::Local, .detail = std::string(why)}));
    }
}

void Multi::Impl::complete(Transfer& transfer, Result result) noexcept {
    // The handler may destroy the transfer; it is not touched afterwards.
    transfer.handler_.on_transfer_done(std::move(result));
}

void Multi::Impl::arm(core::Millis delay) noexcept {
    disarm();
    timer_ = reactor_.arm_timer(delay, *this);
}

void Multi::Impl::disarm() noexcept {
    if (timer_ != net::TimerId{}) {
        reactor_.cancel_timer(timer_);
        timer_ = {};
    }
}

Multi::Impl::Watch& Multi::Impl::acquire(curl_socket_t fd) {
    Watch* watch = nullptr;
    if (idle_.empty()) {
        watches_.push_back(std::make_unique<Watch>(*this));
        watch = watches_.back().get();
    } else {
        watch = idle_.back();
        idle_.pop_back();
    }
    watch->assign(fd);
    return *watch;
}

void Multi::Impl::doom(CURL* easy) noexcept {
    Transfer* const transfer = transfer_of(easy);
    if (transfer != nullptr && std::ranges::find(doomed_, transfer) == doomed_.end()) {
        doomed_.push_back(transfer);
    }
    // The socket is not watched, so nothing else would wake the loop for this transfer.
    arm(core::Millis{0});
}

std::expected<std::unique_ptr<Multi>, MultiError> Multi::create(net::IReactor& reactor,
                                                                std::size_t max_connections) {
    if (!detail::global_init()) {
        return std::unexpected(MultiError::InitFailed);
    }
    // With the threaded resolver getaddrinfo runs on a helper thread and completion is
    // signalled through a socket the reactor watches. Without it, every lookup would block
    // the loop for as long as DNS takes.
    const curl_version_info_data* const info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr || (info->features & CURL_VERSION_ASYNCHDNS) == 0) {
        return std::unexpected(MultiError::SynchronousResolver);
    }
    CURLM* const handle = curl_multi_init();
    if (handle == nullptr) {
        return std::unexpected(MultiError::InitFailed);
    }
    auto impl = std::make_unique<Impl>(reactor, handle);
    if (!impl->configure(max_connections)) {
        return std::unexpected(MultiError::InitFailed);
    }
    return std::make_unique<Multi>(Token{}, std::move(impl));
}

Multi::Multi(Token /*token*/, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Multi::~Multi() = default;

Transfer::Transfer(Token /*token*/, Multi::Impl& multi, std::unique_ptr<detail::Exchange> exchange,
                   ITransferHandler& handler) noexcept
    : multi_(multi), exchange_(std::move(exchange)), handler_(handler) {}

Transfer::~Transfer() {
    multi_.release(*this);
}

std::expected<std::unique_ptr<Transfer>, Failure>
Transfer::start(Multi& multi, const Request& request, ITransferHandler& handler) {
    auto exchange = detail::Exchange::create(request, 0, nullptr);
    if (!exchange) {
        return std::unexpected(std::move(exchange.error()));
    }
    return launch(multi, std::move(*exchange), handler);
}

std::expected<std::unique_ptr<Transfer>, Failure>
Transfer::start_upload(Multi& multi, const Request& request, std::uint64_t length,
                       IBodySource& body, ITransferHandler& handler) {
    auto exchange = detail::Exchange::create(request, length, &body);
    if (!exchange) {
        return std::unexpected(std::move(exchange.error()));
    }
    return launch(multi, std::move(*exchange), handler);
}

std::expected<std::unique_ptr<Transfer>, Failure>
Transfer::launch(Multi& multi, std::unique_ptr<detail::Exchange> exchange,
                 ITransferHandler& handler) {
    auto transfer = std::make_unique<Transfer>(Token{}, *multi.impl_, std::move(exchange), handler);
    if (auto attached = multi.impl_->attach(*transfer); !attached) {
        return std::unexpected(std::move(attached.error()));
    }
    return transfer;
}

void Transfer::resume_body() noexcept {
    exchange_->resume_body();
}

} // namespace infra::curl
