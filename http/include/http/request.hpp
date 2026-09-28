#pragma once

#include "http/method.hpp"
#include "http/status.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace http {

struct HeaderField {
    std::string_view name;
    std::string_view value;
};

// Every view points into the parser that produced it and stays valid until that parser's
// reset_for_next_request().
struct RequestHead {
    Method method = Method::Other;
    // Path and query exactly as sent: not decoded, not normalised.
    std::string_view target;
    int version_minor = 1;
    std::uint64_t content_length = 0;
    bool keep_alive = true;
    std::span<const HeaderField> headers;
};

// The first field with this name, compared case-insensitively. Present-but-empty is "".
// Reading a single-valued field this way is only sound if the parser refuses a second copy of
// it: such a name belongs in kSingleValued in request_parser.cpp.
[[nodiscard]] std::optional<std::string_view> find_header(std::span<const HeaderField> headers,
                                                          std::string_view name) noexcept;

class HeadVerdict {
public:
    [[nodiscard]] static constexpr HeadVerdict accept() noexcept { return HeadVerdict{{}}; }
    [[nodiscard]] static constexpr HeadVerdict reject(Status status) noexcept {
        return HeadVerdict{status};
    }

    [[nodiscard]] constexpr std::optional<Status> rejection() const noexcept { return rejection_; }

private:
    constexpr explicit HeadVerdict(std::optional<Status> rejection) noexcept
        : rejection_(rejection) {}
    std::optional<Status> rejection_;
};

enum class BodyVerdict : std::uint8_t { Continue, Pause };

// Called from inside RequestParser::feed() and resume(), never re-entrantly.
class IRequestSink {
public:
    // A rejection stops the parser, which reports the status to its caller.
    [[nodiscard]] virtual HeadVerdict on_head(const RequestHead& head) noexcept = 0;
    // `bytes` is only valid during the call. Whatever the sink cannot pass on it keeps itself;
    // Pause then stops the parser behind `bytes` until resume().
    [[nodiscard]] virtual BodyVerdict on_body(std::span<const std::byte> bytes) noexcept = 0;
    virtual void on_message_complete() noexcept = 0;

protected:
    IRequestSink() = default;
    IRequestSink(const IRequestSink&) = default;
    IRequestSink(IRequestSink&&) = default;
    IRequestSink& operator=(const IRequestSink&) = default;
    IRequestSink& operator=(IRequestSink&&) = default;
    ~IRequestSink() = default;
};

} // namespace http
