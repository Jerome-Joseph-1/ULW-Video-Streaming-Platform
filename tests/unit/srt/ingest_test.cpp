#include "infra/srt/ingest.hpp"

#include "support/srt_caller.hpp"
#include "support/srt_runtime.hpp"

#include <array>
#include <gtest/gtest.h>
#include <optional>
#include <stop_token>
#include <string>

namespace {

using infra::srt::IngestListener;
using infra::srt::ReadStatus;
using ulw::test::SrtCaller;

constexpr std::string_view kPassphrase = "a passphrase of 24 chars";

IngestListener listener() {
    auto bound = IngestListener::bind(ulw::test::kSrtRuntime->runtime(),
                                      {.host = "127.0.0.1",
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
            std::string payload;
            for (std::size_t i = 0; i < read.bytes; ++i) {
                payload.push_back(static_cast<char>(buffer.at(i)));
            }
            return payload;
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
    const SrtCaller caller;
    ASSERT_TRUE(caller.connect(bound.port(), kPassphrase, "show-1"));
    auto accepted = bound.accept({});
    ASSERT_TRUE(accepted) << accepted.error();
    ASSERT_TRUE(*accepted);
    ASSERT_TRUE(caller.send("mpeg-ts bytes"));
    EXPECT_EQ(read_payload(**accepted), "mpeg-ts bytes");
}

TEST(SrtIngest, RefusesAWrongStreamIdAndStillAcceptsTheRightCallerAfterwards) {
    auto bound = listener();
    const SrtCaller wrong;
    EXPECT_FALSE(wrong.connect(bound.port(), kPassphrase, "other-show"));
    const SrtCaller right;
    ASSERT_TRUE(right.connect(bound.port(), kPassphrase, "show-1"));
    const auto accepted = bound.accept({});
    ASSERT_TRUE(accepted && *accepted);
}

TEST(SrtIngest, RefusesACallerWithoutAStreamId) {
    auto bound = listener();
    const SrtCaller caller;
    EXPECT_FALSE(caller.connect(bound.port(), kPassphrase, ""));
}

TEST(SrtIngest, RefusesAWrongPassphrase) {
    auto bound = listener();
    const SrtCaller caller;
    EXPECT_FALSE(caller.connect(bound.port(), "some other passphrase", "show-1"));
    const SrtCaller right;
    ASSERT_TRUE(right.connect(bound.port(), kPassphrase, "show-1"));
    const auto accepted = bound.accept({});
    ASSERT_TRUE(accepted && *accepted);
}

TEST(SrtIngest, RefusesACallerThatDoesNotEncryptAtAll) {
    auto bound = listener();
    const SrtCaller caller;
    EXPECT_FALSE(caller.connect(bound.port(), "", "show-1"));
}

TEST(SrtIngest, ReportsIdleWhileTheCallerIsQuietAndClosedOnceItIsGone) {
    auto bound = listener();
    std::optional<infra::srt::Session> session;
    {
        const SrtCaller caller;
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
    const std::stop_source stop;
    stop.request_stop();
    const auto accepted = bound.accept(stop.get_token());
    ASSERT_TRUE(accepted);
    EXPECT_FALSE(*accepted);
}

TEST(SrtIngest, StopsListeningOnceItHasItsPublisher) {
    auto bound = listener();
    const std::uint16_t port = bound.port();
    const SrtCaller first;
    ASSERT_TRUE(first.connect(port, kPassphrase, "show-1"));
    const auto accepted = bound.accept({});
    ASSERT_TRUE(accepted && *accepted);
    const SrtCaller second;
    EXPECT_FALSE(second.connect(port, kPassphrase, "show-1"));
}

TEST(SrtIngest, RefusesPassphrasesSrtWouldNotTakeAndAnEmptyStreamId) {
    const auto bind = [](std::string passphrase, std::string stream) {
        return IngestListener::bind(ulw::test::kSrtRuntime->runtime(),
                                    {.host = "127.0.0.1",
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
    const auto bound = IngestListener::bind(ulw::test::kSrtRuntime->runtime(),
                                            {.host = "localhost",
                                             .port = 0,
                                             .passphrase = std::string(kPassphrase),
                                             .stream_id = "show"});
    EXPECT_FALSE(bound);
}

} // namespace
