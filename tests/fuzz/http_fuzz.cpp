// Parses each input twice: once in a single write to a sink that never pauses, and once in
// small writes to a sink that accepts random amounts and pauses at random, with more bytes
// fed while the parser is stopped and stray reset calls. Both must see the same requests,
// byte for byte.
//
// Input: byte 0 picks the write size, byte 1 seeds the pausing; the rest is the stream.

#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/status.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

void check(bool invariant) {
    if (!invariant) {
        __builtin_trap();
    }
}

class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    std::size_t below(std::size_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

private:
    std::uint64_t state_;
};

// Writes what it observes into a transcript and checks, as the events arrive, that the
// parser never over-delivers a body, delivers while paused, or completes a short one.
class FuzzSink final : public http::IRequestSink {
public:
    explicit FuzzSink(std::optional<SplitMix> pauses) noexcept : pauses_(pauses) {}

    // A body cut short by the end of the input is compared as well.
    [[nodiscard]] std::string transcript() const {
        return open_ ? std::format("{}partial {}:{}\n", transcript_, body_.size(), body_)
                     : transcript_;
    }
    [[nodiscard]] bool paused_with_bytes() const noexcept { return !staged_.empty(); }
    [[nodiscard]] bool in_body() const noexcept { return open_; }

    void drain() {
        body_ += staged_;
        staged_.clear();
    }

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        check(!open_ && staged_.empty());
        transcript_ +=
            std::format("head {} {} v{} cl={} ka={}\n", static_cast<int>(head.method), head.target,
                        head.version_minor, head.content_length, head.keep_alive);
        for (const http::HeaderField& f : head.headers) {
            check(http::find_header(head.headers, f.name).has_value());
            transcript_ += std::format("  {}: {}\n", f.name, f.value);
        }
        // Deterministic, so both parses reject the same requests.
        if (head.target.find("reject") != std::string_view::npos) {
            transcript_ += "rejected\n";
            return http::HeadVerdict::reject(http::Status::NotFound);
        }
        open_ = true;
        content_length_ = head.content_length;
        body_.clear();
        return http::HeadVerdict::accept();
    }

    http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override {
        check(open_ && staged_.empty() && !bytes.empty());
        check(body_.size() + bytes.size() <= content_length_);
        const std::size_t taken = pauses_ ? pauses_->below(bytes.size() + 1) : bytes.size();
        for (const std::byte b : bytes.first(taken)) {
            body_.push_back(static_cast<char>(b));
        }
        for (const std::byte b : bytes.subspan(taken)) {
            staged_.push_back(static_cast<char>(b));
        }
        const bool pause_anyway = pauses_ && pauses_->below(4) == 0;
        return staged_.empty() && !pause_anyway ? http::BodyVerdict::Continue
                                                : http::BodyVerdict::Pause;
    }

    void on_message_complete() noexcept override {
        check(open_ && staged_.empty() && body_.size() == content_length_);
        transcript_ += std::format("body {}:{}\nend\n", body_.size(), body_);
        open_ = false;
    }

private:
    std::optional<SplitMix> pauses_;
    std::string transcript_;
    std::string body_;
    std::string staged_;
    std::uint64_t content_length_ = 0;
    bool open_ = false;
};

struct Outcome {
    std::string transcript;
    std::optional<http::ParseError> error;
};

// Drives the parser like a connection: finishes each request at once, answers a recoverable
// rejection and carries on, and stops at the first error that ends the connection.
Outcome parse(std::span<const std::byte> stream, std::size_t write_size,
              std::optional<std::uint64_t> seed) {
    FuzzSink sink{seed ? std::optional{SplitMix{*seed}} : std::nullopt};
    http::RequestParser parser{sink};
    std::optional<SplitMix> chaos = seed ? std::optional{SplitMix{~*seed}} : std::nullopt;
    std::size_t offset = 0;
    const auto next_write = [&] {
        const std::span<const std::byte> piece =
            stream.subspan(offset, std::min(write_size, stream.size() - offset));
        offset += piece.size();
        return piece;
    };

    http::ParseResult r = http::ParseProgress::NeedMore;
    while (true) {
        if (!r) {
            if (r.error().must_close) {
                break;
            }
            parser.reset_for_next_request();
            r = parser.resume();
            continue;
        }
        switch (*r) {
        case http::ParseProgress::NeedMore:
            if (offset == stream.size()) {
                return {.transcript = sink.transcript(), .error = std::nullopt};
            }
            r = parser.feed(next_write());
            break;
        case http::ParseProgress::MessageComplete:
            check(parser.resume() == http::ParseProgress::Paused);
            if (chaos && offset < stream.size() && chaos->below(2) == 0) {
                check(parser.feed(next_write()) == http::ParseProgress::Paused);
            }
            parser.reset_for_next_request();
            r = parser.resume();
            break;
        case http::ParseProgress::Paused:
            check(sink.in_body());
            if (chaos && offset < stream.size() && chaos->below(2) == 0) {
                check(parser.feed(next_write()) == http::ParseProgress::Paused);
            }
            if (chaos && chaos->below(4) == 0) {
                parser.reset_for_next_request();
            }
            sink.drain();
            r = parser.resume();
            break;
        }
    }
    // A connection-ending error is final, whatever arrives or is asked afterwards.
    check(parser.feed(next_write()) == r);
    parser.reset_for_next_request();
    check(parser.resume() == r);
    return {.transcript = sink.transcript(), .error = r.error()};
}

bool is_header_limit_race(const http::ParseError& a, const http::ParseError& b) {
    // Small writes flush a partial field to the size check before llhttp sees the byte that
    // makes it malformed, so the same head may fail as 431 one way and 400 the other.
    const auto either = [](http::Status s) {
        return s == http::Status::BadRequest || s == http::Status::RequestHeaderFieldsTooLarge;
    };
    return either(a.status) && either(b.status);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t kControlBytes = 2;
    // Beyond this a single write could legitimately overflow the held-bytes bound in one
    // parse and not the other.
    if (size < kControlBytes || size - kControlBytes > http::RequestParser::kMaxRetainedBytes) {
        return 0;
    }
    const std::span<const std::byte> input = std::as_bytes(std::span{data, size});
    const std::size_t write_size = std::to_integer<std::size_t>(input[0]) + 1;
    const auto seed = std::to_integer<std::uint64_t>(input[1]);
    const std::span<const std::byte> stream = input.subspan(kControlBytes);

    const Outcome whole = parse(stream, stream.size() + 1, std::nullopt);
    const Outcome split = parse(stream, write_size, seed);

    check(whole.transcript == split.transcript);
    check(whole.error.has_value() == split.error.has_value());
    if (whole.error && split.error) {
        check(whole.error->must_close == split.error->must_close);
        check(whole.error->status == split.error->status ||
              is_header_limit_race(*whole.error, *split.error));
    }
    return 0;
}
