#pragma once

#include "http/method.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/status.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ulw::test {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct RecordedRequest {
    http::Method method = http::Method::Other;
    std::string target;
    int version_minor = -1;
    std::uint64_t content_length = 0;
    bool keep_alive = false;
    Headers headers;
    // One entry per looked-up name, answered by http::find_header().
    std::vector<std::optional<std::string>> found;
    std::string body;
    bool complete = false;

    friend bool operator==(const RecordedRequest&, const RecordedRequest&) = default;
};

// Copies everything out of the parser's views, so nothing it records depends on the parser
// keeping its storage intact after the callback returns.
class RecordingSink final : public http::IRequestSink {
public:
    [[nodiscard]] const std::vector<RecordedRequest>& requests() const noexcept {
        return requests_;
    }

    void look_up(std::vector<std::string> names) { lookups_ = std::move(names); }

    void reject(std::string target, http::Status status) {
        reject_target_ = std::move(target);
        reject_status_ = status;
    }

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        RecordedRequest& r = requests_.emplace_back();
        r.method = head.method;
        r.target = head.target;
        r.version_minor = head.version_minor;
        r.content_length = head.content_length;
        r.keep_alive = head.keep_alive;
        for (const http::HeaderField& f : head.headers) {
            r.headers.emplace_back(f.name, f.value);
        }
        for (const std::string& name : lookups_) {
            const auto value = http::find_header(head.headers, name);
            r.found.push_back(value ? std::optional<std::string>{*value} : std::nullopt);
        }
        if (!reject_target_.empty() && head.target == reject_target_) {
            return http::HeadVerdict::reject(reject_status_);
        }
        return http::HeadVerdict::accept();
    }

    http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override {
        for (const std::byte b : bytes) {
            requests_.back().body.push_back(static_cast<char>(b));
        }
        return http::BodyVerdict::Continue;
    }

    void on_message_complete() noexcept override { requests_.back().complete = true; }

private:
    std::vector<RecordedRequest> requests_;
    std::vector<std::string> lookups_;
    std::string reject_target_;
    http::Status reject_status_ = http::Status::NotFound;
};

inline std::span<const std::byte> bytes_of(std::string_view text) noexcept {
    return std::as_bytes(std::span{text.data(), text.size()});
}

constexpr std::unexpected<http::ParseError> fatal(http::Status status) noexcept {
    return std::unexpected(http::ParseError{.status = status, .must_close = true});
}

constexpr std::unexpected<http::ParseError> recoverable(http::Status status) noexcept {
    return std::unexpected(http::ParseError{.status = status, .must_close = false});
}

// Feeds `input` in `chunk`-sized writes the way a connection would, finishing each request
// as soon as it completes and resuming whenever the parser pauses.
inline http::ParseResult drive(http::RequestParser& parser, std::string_view input,
                               std::size_t chunk) {
    for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
        http::ParseResult r = parser.feed(bytes_of(input.substr(offset, chunk)));
        while (r && *r != http::ParseProgress::NeedMore) {
            if (*r == http::ParseProgress::MessageComplete) {
                parser.reset_for_next_request();
            }
            r = parser.resume();
        }
        if (!r) {
            return r;
        }
    }
    return http::ParseProgress::NeedMore;
}

} // namespace ulw::test
