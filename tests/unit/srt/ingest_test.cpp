#include "infra/srt/ingest.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <srt.h>
#include <stop_token>
#include <string>

namespace {

using infra::srt::IngestConfig;
using infra::srt::IngestListener;
using infra::srt::ReadStatus;

constexpr std::string_view kPassphrase = "a passphrase of 24 chars";

// A publisher, as LiveKit egress is to us: an SRT caller in live mode.
class Caller {
public:
    Caller() : socket_(::srt_create_socket()) {}
    ~Caller() { ::srt_close(socket_); }
    Caller(const Caller&) = delete;
    Caller& operator=(const Caller&) = delete;
    Caller(Caller&&) = delete;
    Caller& operator=(Caller&&) = delete;

    // True when the handshake completed.
    bool connect(std::uint16_t port, std::string_view passphrase, std::string_view stream_id) {
        const int live = SRTT_LIVE;
        // Refusals come back at once, but a listener that is gone never answers.
        const int patience_ms = 1000;
        ::srt_setsockflag(socket_, SRTO_TRANSTYPE, &live, sizeof live);
        ::srt_setsockflag(socket_, SRTO_CONNTIMEO, &patience_ms, sizeof patience_ms);
        if (!passphrase.empty()) {
            ::srt_setsockflag(socket_, SRTO_PASSPHRASE, passphrase.data(),
                              static_cast<int>(passphrase.size()));
        }
        if (!stream_id.empty()) {
            ::srt_setsockflag(socket_, SRTO_STREAMID, stream_id.data(),
                              static_cast<int>(stream_id.size()));
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
        return ::srt_connect(socket_, reinterpret_cast<const sockaddr*>(&address),
                             sizeof address) != SRT_ERROR;
    }

    bool send(std::string_view payload) {
        return ::srt_sendmsg(socket_, payload.data(), static_cast<int>(payload.size()), -1, 0) ==
               static_cast<int>(payload.size());
    }

private:
    SRTSOCKET socket_;
};

IngestListener listener() {
    auto bound = IngestListener::bind({.host = "127.0.0.1",
                                       .port = 0,
                                       .passphrase = std::string(kPassphrase),
                                       .stream_id = "show-1"});
    EXPECT_TRUE(bound) << bound.error();
    return std::move(*bound);
}

// Reads until a payload arrives, however many idle waits that takes.
std::string read_payload(infra::srt::Session& session) {
    std::array<std::byte, infra::srt::kMaxPayload> buffer{};
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto read = session.read(buffer);
        if (read.status == ReadStatus::Data) {
            return {reinterpret_cast<const char*>(buffer.data()), read.bytes};
        }
        if (read.status == ReadStatus::Closed) {
            break;
        }
    }
    return {};
}

TEST(SrtIngest, BindsAnEphemeralPortAndReportsTheOneItGot) {
    const auto bound = listener();
    EXPECT_NE(bound.port(), 0);
}

TEST(SrtIngest, HandsOverTheCallersPayloadWhenPassphraseAndStreamIdMatch) {
    auto bound = listener();
    Caller caller;
    ASSERT_TRUE(caller.connect(bound.port(), kPassphrase, "show-1"));
    auto accepted = bound.accept({});
    ASSERT_TRUE(accepted) << accepted.error();
    ASSERT_TRUE(*accepted);
    ASSERT_TRUE(caller.send("mpeg-ts bytes"));
    EXPECT_EQ(read_payload(**accepted), "mpeg-ts bytes");
}

TEST(SrtIngest, RefusesAWrongStreamIdAndStillAcceptsTheRightCallerAfterwards) {
    auto bound = listener();
    Caller wrong;
    EXPECT_FALSE(wrong.connect(bound.port(), kPassphrase, "other-show"));
    Caller right;
    ASSERT_TRUE(right.connect(bound.port(), kPassphrase, "show-1"));
    const auto accepted = bound.accept({});
    ASSERT_TRUE(accepted && *accepted);
}

TEST(SrtIngest, RefusesACallerWithoutAStreamId) {
    auto bound = listener();
    Caller caller;
    EXPECT_FALSE(caller.connect(bound.port(), kPassphrase, ""));
}

TEST(SrtIngest, RefusesAWrongPassphrase) {
    auto bound = listener();
    Caller caller;
    EXPECT_FALSE(caller.connect(bound.port(), "some other passphrase", "show-1"));
    Caller right;
    ASSERT_TRUE(right.connect(bound.port(), kPassphrase, "show-1"));
    const auto accepted = bound.accept({});
    ASSERT_TRUE(accepted && *accepted);
}

TEST(SrtIngest, RefusesACallerThatDoesNotEncryptAtAll) {
    auto bound = listener();
    Caller caller;
    EXPECT_FALSE(caller.connect(bound.port(), "", "show-1"));
}

TEST(SrtIngest, ReportsIdleWhileTheCallerIsQuietAndClosedOnceItIsGone) {
    auto bound = listener();
    std::optional<infra::srt::Session> session;
    {
        Caller caller;
        ASSERT_TRUE(caller.connect(bound.port(), kPassphrase, "show-1"));
        auto accepted = bound.accept({});
        ASSERT_TRUE(accepted && *accepted);
        session.emplace(std::move(**accepted));
        std::array<std::byte, infra::srt::kMaxPayload> buffer{};
        EXPECT_EQ(session->read(buffer).status, ReadStatus::Idle);
    }
    std::array<std::byte, infra::srt::kMaxPayload> buffer{};
    ReadStatus last = ReadStatus::Idle;
    for (int attempt = 0; attempt < 100 && last != ReadStatus::Closed; ++attempt) {
        last = session->read(buffer).status;
    }
    EXPECT_EQ(last, ReadStatus::Closed);
}

TEST(SrtIngest, ReturnsNoSessionWhenStoppedFirst) {
    auto bound = listener();
    std::stop_source stop;
    stop.request_stop();
    const auto accepted = bound.accept(stop.get_token());
    ASSERT_TRUE(accepted);
    EXPECT_FALSE(*accepted);
}

TEST(SrtIngest, StopsListeningOnceItHasItsPublisher) {
    auto bound = listener();
    const std::uint16_t port = bound.port();
    Caller first;
    ASSERT_TRUE(first.connect(port, kPassphrase, "show-1"));
    const auto accepted = bound.accept({});
    ASSERT_TRUE(accepted && *accepted);
    Caller second;
    EXPECT_FALSE(second.connect(port, kPassphrase, "show-1"));
}

TEST(SrtIngest, RefusesPassphrasesSrtWouldNotTakeAndAnEmptyStreamId) {
    const auto bind = [](std::string passphrase, std::string stream) {
        return IngestListener::bind({.host = "127.0.0.1",
                                     .port = 0,
                                     .passphrase = std::move(passphrase),
                                     .stream_id = std::move(stream)});
    };
    EXPECT_FALSE(bind("too short", "show"));
    EXPECT_FALSE(bind(std::string(80, 'x'), "show"));
    EXPECT_FALSE(bind(std::string(kPassphrase), ""));
    EXPECT_TRUE(bind(std::string(79, 'x'), "show"));
}

TEST(SrtIngest, RefusesAHostNameRatherThanResolvingIt) {
    const auto bound = IngestListener::bind({.host = "localhost",
                                             .port = 0,
                                             .passphrase = std::string(kPassphrase),
                                             .stream_id = "show"});
    EXPECT_FALSE(bound);
}

} // namespace
