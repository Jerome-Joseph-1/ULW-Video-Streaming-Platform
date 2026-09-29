#include "net/transport.hpp"

namespace net {

namespace {

// The reactor calls the protocol's handler directly; this only remembers which connection the
// protocol's calls are about.
class PlainTransport final : public ITransport {
public:
    PlainTransport(IReactor& reactor, ConnId conn) noexcept : reactor_(reactor), conn_(conn) {}

    void start_receiving() noexcept override { reactor_.start_receiving(conn_); }
    void stop_receiving() noexcept override { reactor_.stop_receiving(conn_); }
    void send(std::span<const std::byte> bytes) noexcept override { reactor_.send(conn_, bytes); }
    [[nodiscard]] std::size_t pending_send_bytes() const noexcept override {
        return reactor_.pending_send_bytes(conn_);
    }
    void shutdown_write() noexcept override { reactor_.shutdown_write(conn_); }
    void begin_close() noexcept override { reactor_.begin_close(conn_); }
    [[nodiscard]] bool is_quiescent() const noexcept override {
        return reactor_.is_quiescent(conn_);
    }

private:
    IReactor& reactor_;
    ConnId conn_;
};

class PlainTransports final : public ITransportFactory {
public:
    explicit PlainTransports(IReactor& reactor) noexcept : reactor_(reactor) {}

    [[nodiscard]] std::expected<std::unique_ptr<ITransport>, int>
    attach(os::UniqueFd conn, IStreamHandler& handler) override {
        const auto id = reactor_.attach(std::move(conn), handler);
        if (!id) {
            return std::unexpected(id.error());
        }
        return std::make_unique<PlainTransport>(reactor_, *id);
    }
    void reload(OffloadPool& /*pool*/, IReloadHandler& done) override { done.on_reloaded({}); }
    [[nodiscard]] std::size_t handshakes_in_flight() const noexcept override { return 0; }
    [[nodiscard]] std::uint64_t handshake_failures() const noexcept override { return 0; }

private:
    IReactor& reactor_;
};

} // namespace

std::unique_ptr<ITransportFactory> make_plain_transports(IReactor& reactor) {
    return std::make_unique<PlainTransports>(reactor);
}

} // namespace net
