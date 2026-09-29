#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/status.hpp"

#include "recording_sink.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using http::BodyVerdict;
using http::ParseProgress;
using http::RequestParser;
using ulw::test::bytes_of;

struct Received {
    std::string target;
    std::string body;
    bool complete = false;

    friend bool operator==(const Received&, const Received&) = default;
};

// Stands in for an upload handler whose store takes at most kStoreBytes per attempt: it keeps
// the rest of each fragment in its own staging buffer and pauses the parser until that drains.
class ThrottledSink final : public http::IRequestSink {
public:
    static constexpr std::size_t kStoreBytes = 100;

    [[nodiscard]] const std::vector<Received>& received() const noexcept { return received_; }
    [[nodiscard]] bool delivered_while_paused() const noexcept { return delivered_while_paused_; }
    [[nodiscard]] bool delivered_empty_fragment() const noexcept {
        return delivered_empty_fragment_;
    }
    [[nodiscard]] std::size_t largest_fragment() const noexcept { return largest_fragment_; }

    // One store attempt; true once nothing is staged.
    bool drain_once() {
        const std::size_t n = std::min(kStoreBytes, staging_.size());
        received_.back().body.append(staging_, 0, n);
        staging_.erase(0, n);
        return staging_.empty();
    }

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        received_.push_back({.target = std::string{head.target}, .body = {}, .complete = false});
        return http::HeadVerdict::accept();
    }

    BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override {
        if (!staging_.empty()) {
            delivered_while_paused_ = true;
        }
        if (bytes.empty()) {
            delivered_empty_fragment_ = true;
        }
        largest_fragment_ = std::max(largest_fragment_, bytes.size());
        const std::size_t n = std::min(kStoreBytes, bytes.size());
        for (const std::byte b : bytes.first(n)) {
            received_.back().body.push_back(static_cast<char>(b));
        }
        for (const std::byte b : bytes.subspan(n)) {
            staging_.push_back(static_cast<char>(b));
        }
        return staging_.empty() ? BodyVerdict::Continue : BodyVerdict::Pause;
    }

    void on_message_complete() noexcept override {
        if (!staging_.empty()) {
            delivered_while_paused_ = true;
        }
        received_.back().complete = true;
    }

private:
    std::vector<Received> received_;
    std::string staging_;
    bool delivered_while_paused_ = false;
    bool delivered_empty_fragment_ = false;
    std::size_t largest_fragment_ = 0;
};

// Byte i is (i * 31 + 7) mod 251: no period that divides a chunk size, so a dropped,
// duplicated or reordered run of bytes cannot line up with the expected pattern.
std::string patterned(std::size_t size) {
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>((i * 31 + 7) % 251);
    }
    return out;
}

std::string patch(std::string_view target, std::string_view body) {
    return std::format("PATCH {} HTTP/1.1\r\nHost: a\r\nContent-Length: {}\r\n\r\n{}", target,
                       body.size(), body);
}

struct ThrottledRun {
    std::vector<Received> received;
    http::ParseResult last;
    bool delivered_while_paused = false;
    bool delivered_empty_fragment = false;
};

// Feeds `input` in `chunk`-sized receives. Whenever the parser pauses, one more receive is
// still in flight and arrives before the sink's store drains, as it would with a real socket.
ThrottledRun run_throttled(std::string_view input, std::size_t chunk) {
    ThrottledSink sink;
    RequestParser parser{sink};
    std::size_t offset = 0;
    const auto next_receive = [&] {
        const std::string_view piece = input.substr(offset, chunk);
        offset += piece.size();
        return bytes_of(piece);
    };

    http::ParseResult r = ParseProgress::NeedMore;
    while (r) {
        if (*r == ParseProgress::NeedMore) {
            if (offset == input.size()) {
                break;
            }
            r = parser.feed(next_receive());
        } else if (*r == ParseProgress::MessageComplete) {
            parser.reset_for_next_request();
            r = parser.resume();
        } else {
            if (offset < input.size()) {
                r = parser.feed(next_receive());
                if (!r || *r != ParseProgress::Paused) {
                    break;
                }
            }
            while (!sink.drain_once()) {
            }
            r = parser.resume();
        }
    }
    return {.received = sink.received(),
            .last = r,
            .delivered_while_paused = sink.delivered_while_paused(),
            .delivered_empty_fragment = sink.delivered_empty_fragment()};
}

TEST(Backpressure, ThrottledBodyArrivesByteExactAtEveryReceiveSize) {
    const std::string big = patterned(8192);
    const std::string small = patterned(300);
    const std::string input = patch("/api/v1/uploads/a", big) + patch("/api/v1/uploads/b", small) +
                              "GET /api/v1/videos/v HTTP/1.1\r\nHost: a\r\n\r\n";
    const std::vector<Received> expected{
        {.target = "/api/v1/uploads/a", .body = big, .complete = true},
        {.target = "/api/v1/uploads/b", .body = small, .complete = true},
        {.target = "/api/v1/videos/v", .body = "", .complete = true},
    };

    for (const std::size_t chunk : std::array<std::size_t, 5>{1, 7, 100, 4096, input.size()}) {
        const ThrottledRun run = run_throttled(input, chunk);
        EXPECT_EQ(run.last, ParseProgress::NeedMore) << chunk;
        EXPECT_FALSE(run.delivered_while_paused) << chunk;
        EXPECT_FALSE(run.delivered_empty_fragment) << chunk;
        EXPECT_TRUE(run.received == expected) << "receive size " << chunk;
    }
}

TEST(Backpressure, PauseOnTheFinalBodyByteCompletesOnResume) {
    ThrottledSink sink;
    RequestParser parser{sink};
    const std::string body = patterned(ThrottledSink::kStoreBytes + 1);

    // Nothing follows the body, so resume() has no bytes left to parse and must still finish.
    ASSERT_EQ(parser.feed(bytes_of(patch("/u", body))), ParseProgress::Paused);
    EXPECT_FALSE(sink.received().back().complete);
    ASSERT_TRUE(sink.drain_once());
    EXPECT_EQ(parser.resume(), ParseProgress::MessageComplete);
    EXPECT_EQ(sink.received().back().body, body);
    EXPECT_TRUE(sink.received().back().complete);
}

TEST(Backpressure, ResumingWithNothingNewDeliversNoEmptyFragment) {
    ThrottledSink sink;
    RequestParser parser{sink};
    const std::string request = patch("/u", patterned(2 * ThrottledSink::kStoreBytes));

    // The write ends inside the body, so llhttp still has the body span open when it pauses.
    ASSERT_EQ(parser.feed(bytes_of(request).first(request.size() - 1)), ParseProgress::Paused);
    ASSERT_TRUE(sink.drain_once());
    EXPECT_EQ(parser.resume(), ParseProgress::NeedMore);
    EXPECT_FALSE(sink.delivered_empty_fragment());
    EXPECT_EQ(parser.feed(bytes_of(request).last(1)), ParseProgress::MessageComplete);
}

TEST(Backpressure, TheBodyOfAReceiveReachesTheSinkWholePastTheHeadBudget) {
    ThrottledSink sink;
    RequestParser parser{sink};
    // One 64 KiB receive holding a head and the start of a large body: more than the head
    // budget, so the parser cannot hand llhttp all of it before the head has ended.
    const std::string body = patterned((64 * 1024) - 200);
    const std::string request = patch("/u", body);
    ASSERT_GT(request.size(), RequestParser::kMaxHeadBytes);

    ASSERT_EQ(parser.feed(bytes_of(request)), ParseProgress::Paused);
    // Paused on its first fragment, the sink holds the whole body and the parser holds nothing,
    // so the receive sits in one buffer, not in two.
    EXPECT_EQ(sink.largest_fragment(), body.size());
    while (!sink.drain_once()) {
    }
    EXPECT_EQ(parser.resume(), ParseProgress::MessageComplete);
    EXPECT_EQ(sink.received().back().body, body);
}

TEST(Backpressure, ResumeWithoutPauseIsHarmless) {
    ThrottledSink sink;
    RequestParser parser{sink};

    EXPECT_EQ(parser.resume(), ParseProgress::NeedMore);
    EXPECT_EQ(parser.feed(bytes_of("GET / HTTP/1.1\r\nHost: a\r\n\r\n")),
              ParseProgress::MessageComplete);
}

TEST(Backpressure, BytesArrivingWhilePausedAreBounded) {
    ThrottledSink sink;
    RequestParser parser{sink};
    const std::string head =
        std::format("PATCH /u HTTP/1.1\r\nHost: a\r\nContent-Length: {}\r\n\r\n",
                    RequestParser::kMaxContentLength);
    ASSERT_EQ(parser.feed(bytes_of(head + patterned(ThrottledSink::kStoreBytes + 1))),
              ParseProgress::Paused);

    const std::string in_flight(RequestParser::kMaxRetainedBytes, 'x');
    EXPECT_EQ(parser.feed(bytes_of(in_flight)), ParseProgress::Paused);
    EXPECT_EQ(parser.feed(bytes_of("x")), ulw::test::fatal(http::Status::ContentTooLarge));
}

TEST(Backpressure, StrayResetWhilePausedCannotSpliceTheBodyIntoANewRequest) {
    ThrottledSink sink;
    RequestParser parser{sink};
    const std::string_view smuggled = "GET /admin HTTP/1.1\r\nHost: a\r\n\r\n";
    const std::string body = patterned(ThrottledSink::kStoreBytes + 50) + std::string{smuggled};
    const std::string request = patch("/u", body);
    const std::size_t split = request.size() - smuggled.size();
    ASSERT_EQ(parser.feed(bytes_of(request).first(split)), ParseProgress::Paused);

    parser.reset_for_next_request();
    ASSERT_TRUE(sink.drain_once());
    EXPECT_EQ(parser.resume(), ParseProgress::NeedMore);
    EXPECT_EQ(parser.feed(bytes_of(smuggled)), ParseProgress::MessageComplete);

    ASSERT_EQ(sink.received().size(), 1U);
    EXPECT_EQ(sink.received()[0].body, body);
}

} // namespace
