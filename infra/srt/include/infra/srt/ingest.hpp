#pragma once

#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

namespace infra::srt {

struct IngestConfig {
    // A numeric address; nothing on this path may wait on a resolver.
    std::string host;
    // A UDP port; 0 takes an ephemeral one.
    std::uint16_t port = 0;
    // Callers that do not present it, and hold this passphrase, are refused during the
    // handshake. SRT accepts 10 to 79 characters.
    std::string passphrase;
    std::string stream_id;
};

inline constexpr std::size_t kMinPassphrase = 10;
inline constexpr std::size_t kMaxPassphrase = 79;

enum class ReadStatus : std::uint8_t {
    // `bytes` of the payload were read.
    Data,
    // Nothing arrived within kReadWait.
    Idle,
    // The caller is gone: it closed, or SRT found it silent for its idle timeout.
    Closed,
};

struct ReadResult {
    ReadStatus status = ReadStatus::Closed;
    std::size_t bytes = 0;
};

// How long a read waits for the next payload before reporting Idle, so a caller polling for
// a stop request is never more than this late.
inline constexpr core::Millis kReadWait{100};

// A live SRT payload is at most this many bytes: SRT's default packet size less its headers,
// for an MTU of 1500 (1500 - 20 IP - 8 UDP - 16 SRT = 1456).
inline constexpr std::size_t kMaxPayload = 1456;

// One connected caller. Only the payload leaves: what it carries (MPEG-TS from LiveKit
// egress) is not looked at here.
class Session {
public:
    ~Session();
    Session(Session&& other) noexcept;
    Session& operator=(Session&&) = delete;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] ReadResult read(std::span<std::byte> out) const;

private:
    friend class IngestListener;
    explicit Session(int socket) noexcept : socket_(socket) {}

    int socket_;
};

// An SRT listener for one publisher. It terminates the protocol, the handshake, the
// encryption and the retransmission, on UDP, in this process; ffmpeg never touches the network.
class IngestListener {
public:
    [[nodiscard]] static std::expected<IngestListener, std::string>
    bind(const IngestConfig& config);

    ~IngestListener();
    IngestListener(IngestListener&& other) noexcept;
    IngestListener& operator=(IngestListener&&) = delete;
    IngestListener(const IngestListener&) = delete;
    IngestListener& operator=(const IngestListener&) = delete;

    // The port bound, which is the requested one unless that was 0.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Waits for the publisher; nullopt when `stop` fires first. Callers refused for a wrong
    // passphrase or stream id are not returned: the wait goes on. The listening socket closes
    // with the first caller accepted, so a second publisher finds nobody.
    [[nodiscard]] std::expected<std::optional<Session>, std::string>
    accept(const std::stop_token& stop);

private:
    IngestListener(int socket, std::uint16_t port, std::unique_ptr<std::string> stream_id) noexcept;

    int socket_;
    std::uint16_t port_;
    // The listener callback reads it from libsrt's threads, so its address must not move.
    std::unique_ptr<std::string> stream_id_;
};

} // namespace infra::srt
