#pragma once

#include "net/reactor.hpp"
#include "os/unique_fd.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace net {

// A connection's byte stream as the protocol above it sees it: plaintext both ways, whether
// the socket carries it as is or inside TLS (ADR-0001). The operations mean what their
// IReactor namesakes mean for the connection, and the handler given to attach() receives
// plaintext through the same IStreamHandler callbacks, so the protocol cannot tell which
// transport it holds.
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual void start_receiving() noexcept = 0;
    // Exact, as on the reactor: no on_data follows until start_receiving. Whatever the socket
    // delivered past that point stays buffered below the protocol, bounded by one receive.
    virtual void stop_receiving() noexcept = 0;
    // Only after the handler has received data: a TLS server has no keys to send with before.
    virtual void send(std::span<const std::byte> bytes) noexcept = 0;
    [[nodiscard]] virtual std::size_t pending_send_bytes() const noexcept = 0;
    // Ends the sending direction after everything already sent; for TLS, close_notify first.
    virtual void shutdown_write() noexcept = 0;
    virtual void begin_close() noexcept = 0;
    // The transport may be destroyed once this holds, and not before.
    [[nodiscard]] virtual bool is_quiescent() const noexcept = 0;
};

class OffloadPool;

class IReloadHandler {
public:
    virtual ~IReloadHandler() = default;
    // On the loop. The error is a line fit for a log.
    virtual void on_reloaded(const std::expected<void, std::string>& result) noexcept = 0;
};

// Makes the transport for each accepted socket. Single-threaded like the reactor it uses.
class ITransportFactory {
public:
    virtual ~ITransportFactory() = default;

    // `handler` must outlive the transport. The connection starts out not receiving.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ITransport>, int>
    attach(os::UniqueFd conn, IStreamHandler& handler) = 0;
    // Rereads the certificate and key on `pool`, because file reads would stall the loop, and
    // reports to `done` on the loop. On failure every connection, new ones included, keeps the
    // previous pair. Asked for while one is running, it runs once more after that one. The
    // pool must be destroyed before the factory and `done`.
    virtual void reload(OffloadPool& pool, IReloadHandler& done) = 0;
    [[nodiscard]] virtual std::size_t handshakes_in_flight() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ITransportFactory> make_plain_transports(IReactor& reactor);

struct TlsFiles {
    // PEM: the server certificate first, then the intermediates a client needs to reach a root.
    std::string certificate_chain;
    std::string private_key;
};

// Fails with a line fit for a startup log when the files are unreadable or do not match.
[[nodiscard]] std::expected<std::unique_ptr<ITransportFactory>, std::string>
make_tls_transports(IReactor& reactor, TlsFiles files);

} // namespace net
