#include "ingest.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <netdb.h>
#include <poll.h>
#include <system_error>
#include <unistd.h>

namespace live {

namespace {

// How long a wait for a publisher goes before it looks at the stop token again.
constexpr int kStopCheckMs = 100;

std::string errno_text(int error) {
    return std::generic_category().message(error);
}

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

// The sockets API's own casts: sockaddr_storage holds either family.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
std::uint16_t bound_port(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace

std::expected<IngestListener, std::string> IngestListener::bind(std::string_view host,
                                                                std::uint16_t port) {
    // A numeric address only: nothing on this path may wait on a resolver.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE | AI_NUMERICSERV;
    AddressList addresses;
    const std::string host_text(host);
    const std::string port_text = std::to_string(port);
    if (const int rc = ::getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &addresses.head);
        rc != 0) {
        return std::unexpected("address " + host_text + ": " + ::gai_strerror(rc));
    }
    os::UniqueFd fd(::socket(addresses.head->ai_family, addresses.head->ai_socktype | SOCK_CLOEXEC,
                             addresses.head->ai_protocol));
    if (!fd) {
        return std::unexpected("socket: " + errno_text(errno));
    }
    // A restarted packager binds the port its crashed predecessor left in TIME_WAIT.
    const int one = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        return std::unexpected("SO_REUSEADDR: " + errno_text(errno));
    }
    if (::bind(fd.get(), addresses.head->ai_addr, addresses.head->ai_addrlen) != 0) {
        return std::unexpected("bind " + host_text + ":" + port_text + ": " + errno_text(errno));
    }
    if (::listen(fd.get(), 1) != 0) {
        return std::unexpected("listen: " + errno_text(errno));
    }
    const std::uint16_t actual = bound_port(fd.get());
    return IngestListener(std::move(fd), actual);
}

std::expected<os::UniqueFd, std::string> IngestListener::accept(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        pollfd waiting{.fd = fd_.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&waiting, 1, kStopCheckMs);
        if (ready < 0 && errno != EINTR) {
            return std::unexpected("poll: " + errno_text(errno));
        }
        if (ready <= 0) {
            continue;
        }
        os::UniqueFd connection(::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!connection) {
            // The publisher gave up between the poll and the accept.
            if (errno == EAGAIN || errno == ECONNABORTED || errno == EINTR) {
                continue;
            }
            return std::unexpected("accept: " + errno_text(errno));
        }
        fd_.reset();
        return connection;
    }
    return os::UniqueFd();
}

} // namespace live
