#pragma once

#include "core/ports/clock.hpp"
#include "net/reactor.hpp"

#include "send_queue.hpp"
#include "timing_wheel.hpp"

#include <array>
#include <memory>
#include <vector>

namespace net::detail {

class EpollReactor final : public IReactor {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<EpollReactor>, int>
    create(core::ports::IClock& clock, std::size_t max_fds);

    EpollReactor(core::ports::IClock& clock, os::UniqueFd epfd, std::size_t max_fds);
    ~EpollReactor() override;
    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    [[nodiscard]] std::expected<void, int> listen(os::UniqueFd listener,
                                                  IAcceptHandler& handler) override;
    void stop_listening() noexcept override;
    [[nodiscard]] std::expected<ConnId, int> attach(os::UniqueFd conn,
                                                    IStreamHandler& handler) override;
    void start_receiving(ConnId conn) noexcept override;
    void stop_receiving(ConnId conn) noexcept override;
    void send(ConnId conn, std::span<const std::byte> bytes) noexcept override;
    [[nodiscard]] std::size_t pending_send_bytes(ConnId conn) const noexcept override;
    void shutdown_write(ConnId conn) noexcept override;
    void begin_close(ConnId conn) noexcept override;
    [[nodiscard]] bool is_quiescent(ConnId conn) const noexcept override;
    [[nodiscard]] std::expected<void, int> watch(int fd, Interest interest,
                                                 IReadyHandler& handler) override;
    void unwatch(int fd) noexcept override;
    TimerId arm_timer(core::Millis delay, ITimerHandler& handler) override;
    void cancel_timer(TimerId timer) noexcept override;
    [[nodiscard]] core::MonoTime now() const noexcept override { return now_; }
    int run_once(core::Millis max_wait) override;

private:
    enum class Kind : std::uint8_t { Free, Stream, Watch, Listener };

    struct Slot {
        Kind kind = Kind::Free;
        bool in_set = false;
        bool receiving = false;
        bool failed = false;
        // The peer hung up while we were not receiving. The descriptor is out of the set, where
        // the hangup would be reported on every wait, until start_receiving reads what is left.
        bool hung_up = false;
        bool eof = false;
        bool shut_pending = false;
        bool accept_paused = false;
        std::uint32_t gen = 1;
        std::uint32_t events = 0;
        os::UniqueFd owned;
        IStreamHandler* stream = nullptr;
        IReadyHandler* ready = nullptr;
        IAcceptHandler* acceptor = nullptr;
        ByteQueue sendq;
    };

    // Re-enables listeners paused by descriptor exhaustion.
    struct AcceptRetry final : ITimerHandler {
        EpollReactor* self = nullptr;
        TimerId timer;
        void on_timeout() noexcept override;
    };

    [[nodiscard]] auto* stream_slot(this auto& self, ConnId conn) noexcept {
        using SlotPtr = decltype(&self.slots_[0]);
        if (conn.fd < 0 || static_cast<std::size_t>(conn.fd) >= self.slots_.size()) {
            return SlotPtr{nullptr};
        }
        auto& s = self.slots_[static_cast<std::size_t>(conn.fd)];
        return s.kind == Kind::Stream && s.gen == conn.gen ? &s : SlotPtr{nullptr};
    }
    [[nodiscard]] bool alive(int fd, std::uint32_t gen) const noexcept;
    [[nodiscard]] static std::uint32_t wanted_events(const Slot& s) noexcept;
    void update_events(int fd, Slot& s) noexcept;
    void remove_from_set(int fd, Slot& s) noexcept;
    void release(Slot& s) noexcept;
    void disable(int fd, Slot& s) noexcept;
    void fail(int fd, Slot& s, int err) noexcept;

    void dispatch(std::uint64_t token, std::uint32_t events) noexcept;
    void on_stream_event(int fd, Slot& s, std::uint32_t events) noexcept;
    void read_ready(int fd, Slot& s) noexcept;
    void flush(int fd, Slot& s) noexcept;
    void on_listener_event(int fd, Slot& s) noexcept;
    void pause_accepting(int fd, Slot& s) noexcept;
    void run_deferred() noexcept;

    core::ports::IClock& clock_;
    os::UniqueFd epfd_;
    std::vector<Slot> slots_;
    TimingWheel wheel_;
    ChunkPool pool_;
    core::MonoTime now_;
    // One buffer serves every connection: it is lent only for the duration of on_data.
    using ReadBuffer = std::array<std::byte, std::size_t{64} * 1024>;
    std::unique_ptr<ReadBuffer> read_buf_;
    std::vector<int> listeners_;
    // Send failures are reported on the next dispatch, never from inside send(): the caller
    // is usually mid-callback and must not be re-entered.
    struct DeferredError {
        ConnId conn;
        int err = 0;
    };
    std::vector<DeferredError> deferred_errors_;
    AcceptRetry accept_retry_;
};

} // namespace net::detail
