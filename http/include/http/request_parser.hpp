#pragma once

#include "http/request.hpp"
#include "http/status.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace http {

enum class ParseProgress : std::uint8_t {
    // Every byte was consumed and more are needed.
    NeedMore,
    // Bytes are being held back; nothing was parsed.
    Paused,
    // A request ended and the parser stopped behind it. Finish that request, then
    // reset_for_next_request() and resume() to parse whatever was pipelined after it.
    MessageComplete,
};

struct ParseError {
    Status status;
    // No request boundary to continue from, so the connection cannot carry another request.
    bool must_close;

    friend bool operator==(const ParseError&, const ParseError&) = default;
};

using ParseResult = std::expected<ParseProgress, ParseError>;

// One per connection, reused across keep-alive requests. Bytes in, sink events out; every
// limit is enforced while the bytes arrive, before the sink sees a head that breaks it.
class RequestParser {
public:
    // Apache's LimitRequestLine is 8190; a presigned playlist query is about 1.5 KiB.
    static constexpr std::size_t kMaxTargetBytes = std::size_t{8} * 1024;
    // Names plus values. Browsers stay under 8 KiB; the rest is room for a signed bearer token.
    static constexpr std::size_t kMaxHeaderBytes = std::size_t{16} * 1024;
    // Apache's LimitRequestFields default; a browser sends about 20.
    static constexpr std::size_t kMaxHeaderCount = 100;
    // Only an upload PATCH carries a real body, and it carries one chunk. Chunks are capped at
    // 16 MiB so a dropped connection costs at most that much re-upload.
    static constexpr std::uint64_t kMaxContentLength = std::uint64_t{16} * 1024 * 1024;
    // Bytes that arrive while paused: the rest of the 64 KiB receive buffer that paused us,
    // plus up to three more receives already in flight when the reactor is told to stop.
    static constexpr std::size_t kMaxRetainedBytes = std::size_t{4} * 64 * 1024;

    explicit RequestParser(IRequestSink& sink);
    ~RequestParser();
    RequestParser(const RequestParser&) = delete;
    RequestParser& operator=(const RequestParser&) = delete;
    RequestParser(RequestParser&&) = delete;
    RequestParser& operator=(RequestParser&&) = delete;

    // While paused, the bytes are retained in order and parsed by the next resume().
    [[nodiscard]] ParseResult feed(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] ParseResult resume() noexcept;
    // Only acts after MessageComplete or a rejection that left the connection usable;
    // anywhere else it is ignored, so a stray call can never splice a body into a new request.
    void reset_for_next_request() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace http
