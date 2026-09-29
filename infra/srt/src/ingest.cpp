#include "infra/srt/ingest.hpp"

#include <sys/socket.h>

#include <array>
#include <cstring>
#include <netdb.h>
#include <srt.h>
#include <utility>

namespace infra::srt {

namespace {

// SRT's reject codes above SRT_REJC_PREDEFINED are the HTTP status codes, so 401.
constexpr int kUnauthorized = SRT_REJC_PREDEFINED + 401;

struct AddressList {
    addrinfo* head = nullptr;
    ~AddressList() {
        if (head != nullptr) {
            ::freeaddrinfo(head);
        }
    }
    AddressList() = default;
    AddressList(const AddressList&) = delete;
    AddressList& operator=(const AddressList&) = delete;
    AddressList(AddressList&&) = delete;
    AddressList& operator=(AddressList&&) = delete;
};

std::string last_error(std::string_view what) {
    return std::string(what) + ": " + ::srt_getlasterror_str();
}

// libsrt calls this from its own thread during the handshake, before the caller is accepted.
// -1 refuses the caller.
int check_stream_id(void* opaque, SRTSOCKET refused, int /*hs_version*/, const sockaddr* /*peer*/,
                    const char* stream_id) {
    const auto& expected = *static_cast<const std::string*>(opaque);
    if (stream_id != nullptr && expected == stream_id) {
        return 0;
    }
    ::srt_setrejectreason(refused, kUnauthorized);
    return -1;
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
std::uint16_t bound_port(SRTSOCKET socket) {
    sockaddr_storage address{};
    int length = sizeof address;
    if (::srt_getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == SRT_ERROR) {
        return 0;
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

// A wait on one socket becoming readable, which for a listener means a caller to accept.
class Poller {
public:
    explicit Poller(SRTSOCKET socket) : id_(::srt_epoll_create()) {
        const int events = static_cast<int>(SRT_EPOLL_IN) | static_cast<int>(SRT_EPOLL_ERR);
        if (id_ >= 0 && ::srt_epoll_add_usock(id_, socket, &events) == SRT_ERROR) {
            ::srt_epoll_release(id_);
            id_ = -1;
        }
    }
    ~Poller() {
        if (id_ >= 0) {
            ::srt_epoll_release(id_);
        }
    }
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;
    Poller(Poller&&) = delete;
    Poller& operator=(Poller&&) = delete;
    [[nodiscard]] bool ok() const noexcept { return id_ >= 0; }
    [[nodiscard]] int id() const noexcept { return id_; }

private:
    int id_;
};

bool set_option(SRTSOCKET socket, SRT_SOCKOPT flag, const void* value, int size) {
    return ::srt_setsockflag(socket, flag, value, size) != SRT_ERROR;
}

} // namespace

std::expected<Runtime, std::string> Runtime::start() {
    if (::srt_startup() < 0) {
        return std::unexpected(last_error("srt startup"));
    }
    // Handshake refusals and the like are logged by libsrt at warning level on stderr, one
    // block of lines for every stray packet a port scan sends; only real faults are wanted.
    ::srt_setloglevel(LOG_CRIT);
    return Runtime();
}

Runtime::~Runtime() {
    if (owns_) {
        ::srt_cleanup();
    }
}

Runtime::Runtime(Runtime&& other) noexcept : owns_(std::exchange(other.owns_, false)) {}

Session::~Session() {
    if (socket_ != SRT_INVALID_SOCK) {
        ::srt_close(socket_);
    }
}

Session::Session(Session&& other) noexcept
    : socket_(std::exchange(other.socket_, SRT_INVALID_SOCK)) {}

ReadResult Session::read(std::span<std::byte> out) const {
    const int n =
        ::srt_recvmsg(socket_, reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()));
    if (n > 0) {
        return {.status = ReadStatus::Data, .bytes = static_cast<std::size_t>(n)};
    }
    // A blocking read that outwaits its timeout reports SRT_ETIMEOUT; nothing yet from a
    // non-blocking one, SRT_EASYNCRCV.
    const int why = ::srt_getlasterror(nullptr);
    if (n == 0 || why == SRT_ETIMEOUT || why == SRT_EASYNCRCV) {
        ::srt_clearlasterror();
        return {.status = ReadStatus::Idle, .bytes = 0};
    }
    return {.status = ReadStatus::Closed, .bytes = 0};
}

IngestListener::IngestListener(int socket, std::uint16_t port,
                               std::unique_ptr<std::string> stream_id) noexcept
    : socket_(socket), port_(port), stream_id_(std::move(stream_id)) {}

IngestListener::IngestListener(IngestListener&& other) noexcept
    : socket_(std::exchange(other.socket_, SRT_INVALID_SOCK)), port_(other.port_),
      stream_id_(std::move(other.stream_id_)) {}

IngestListener::~IngestListener() {
    if (socket_ != SRT_INVALID_SOCK) {
        ::srt_close(socket_);
    }
}

std::expected<IngestListener, std::string> IngestListener::bind(const Runtime& /*runtime*/,
                                                                const IngestConfig& config) {
    if (config.passphrase.size() < kMinPassphrase || config.passphrase.size() > kMaxPassphrase) {
        return std::unexpected("passphrase must be 10 to 79 characters");
    }
    if (config.stream_id.empty()) {
        return std::unexpected("stream id is empty");
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE | AI_NUMERICSERV;
    AddressList addresses;
    const std::string port_text = std::to_string(config.port);
    if (const int rc =
            ::getaddrinfo(config.host.c_str(), port_text.c_str(), &hints, &addresses.head);
        rc != 0) {
        return std::unexpected("address " + config.host + ": " + ::gai_strerror(rc));
    }
    const SRTSOCKET socket = ::srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        return std::unexpected(last_error("socket"));
    }
    IngestListener listener(socket, 0, std::make_unique<std::string>(config.stream_id));
    const int transtype = SRTT_LIVE;
    // Live mode, and callers are refused unless they encrypt with our passphrase (the default
    // enforcement, spelled out: a caller without one must not get in).
    const int enforce = 1;
    // Non-blocking, so that accept() after the poller says a caller is waiting cannot block.
    const int no_wait = 0;
    if (!set_option(socket, SRTO_RCVSYN, &no_wait, sizeof no_wait) ||
        !set_option(socket, SRTO_TRANSTYPE, &transtype, sizeof transtype) ||
        !set_option(socket, SRTO_PASSPHRASE, config.passphrase.data(),
                    static_cast<int>(config.passphrase.size())) ||
        !set_option(socket, SRTO_ENFORCEDENCRYPTION, &enforce, sizeof enforce)) {
        return std::unexpected(last_error("options"));
    }
    if (::srt_listen_callback(socket, &check_stream_id, listener.stream_id_.get()) == SRT_ERROR) {
        return std::unexpected(last_error("listen callback"));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
    if (::srt_bind(socket, addresses.head->ai_addr, static_cast<int>(addresses.head->ai_addrlen)) ==
        SRT_ERROR) {
        return std::unexpected(last_error("bind " + config.host + ":" + port_text));
    }
    if (::srt_listen(socket, 1) == SRT_ERROR) {
        return std::unexpected(last_error("listen"));
    }
    listener.port_ = bound_port(socket);
    return listener;
}

std::expected<std::optional<Session>, std::string>
IngestListener::accept(const std::stop_token& stop) {
    const Poller poller(socket_);
    if (!poller.ok()) {
        return std::unexpected(last_error("epoll"));
    }
    while (!stop.stop_requested()) {
        // Asked before waiting: libsrt raises the readiness event when a caller arrives, and
        // one that arrived before the poller existed is only found by asking.
        const SRTSOCKET accepted = ::srt_accept(socket_, nullptr, nullptr);
        if (accepted != SRT_INVALID_SOCK) {
            Session session(accepted);
            // The listener's options are inherited, its non-blocking mode too; reads block up
            // to kReadWait.
            const int wait = static_cast<int>(kReadWait.count());
            const int blocking = 1;
            if (!set_option(accepted, SRTO_RCVSYN, &blocking, sizeof blocking) ||
                !set_option(accepted, SRTO_RCVTIMEO, &wait, sizeof wait)) {
                return std::unexpected(last_error("read timeout"));
            }
            ::srt_close(socket_);
            socket_ = SRT_INVALID_SOCK;
            return std::optional<Session>(std::move(session));
        }
        std::array<SRTSOCKET, 1> ready{};
        int ready_count = static_cast<int>(ready.size());
        const int rc = ::srt_epoll_wait(poller.id(), ready.data(), &ready_count, nullptr, nullptr,
                                        kReadWait.count(), nullptr, nullptr, nullptr, nullptr);
        if (rc == SRT_ERROR && ::srt_getlasterror(nullptr) != SRT_ETIMEOUT) {
            return std::unexpected(last_error("epoll wait"));
        }
        ::srt_clearlasterror();
    }
    return std::nullopt;
}

} // namespace infra::srt
