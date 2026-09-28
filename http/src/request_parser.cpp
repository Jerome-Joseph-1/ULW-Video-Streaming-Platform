#include "http/request_parser.hpp"

#include "http/method.hpp"
#include "http/request.hpp"
#include "http/status.hpp"

#include "ascii.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <llhttp.h>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace http {

namespace {

enum class State : std::uint8_t {
    Parsing,
    // A request ended or was rejected; its views stay valid until reset_for_next_request().
    AwaitingReset,
    // Reset, with retained bytes that must be parsed before anything fed later.
    AwaitingResume,
    // The last request was not keep-alive: any further byte is an error.
    Closed,
    Failed,
};

// llhttp_execute() wants a valid pointer even for an empty input.
constexpr char kNoInput = 0;

constexpr ParseError fatal(Status status) noexcept {
    return {.status = status, .must_close = true};
}

const char* as_chars(std::span<const std::byte> bytes) noexcept {
    // std::byte and char may alias; llhttp speaks char.
    return reinterpret_cast<const char*>(bytes.data()); // NOLINT(*-reinterpret-cast)
}

Method to_method(std::uint8_t method) noexcept {
    switch (method) {
    case HTTP_GET:
        return Method::Get;
    case HTTP_HEAD:
        return Method::Head;
    case HTTP_POST:
        return Method::Post;
    case HTTP_PUT:
        return Method::Put;
    case HTTP_PATCH:
        return Method::Patch;
    case HTTP_DELETE:
        return Method::Delete;
    case HTTP_OPTIONS:
        return Method::Options;
    default:
        return Method::Other;
    }
}

// llhttp strips leading whitespace from a value but keeps trailing whitespace.
std::string_view trim_ows(std::string_view value) noexcept {
    constexpr std::string_view kOws = " \t";
    const std::size_t first = value.find_first_not_of(kOws);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(kOws) - first + 1);
}

std::optional<std::uint64_t> parse_content_length(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [stop, ec] = std::from_chars(first, last, value);
    if (text.empty() || ec != std::errc{} || stop != last) {
        return std::nullopt;
    }
    return value;
}

bool append(std::span<char> arena, std::size_t& used, std::string_view fragment) noexcept {
    if (fragment.size() > arena.size() - used) {
        return false;
    }
    std::ranges::copy(fragment, arena.subspan(used).begin());
    used += fragment.size();
    return true;
}

} // namespace

class RequestParser::Impl {
public:
    explicit Impl(IRequestSink& sink) noexcept : sink_(sink) {
        llhttp_init(&parser_, HTTP_REQUEST, &settings());
        parser_.data = this;
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() = default;

    ParseResult feed(std::span<const std::byte> bytes) noexcept {
        switch (state_) {
        case State::Parsing:
            return execute(bytes, /*from_tail=*/false);
        case State::AwaitingReset:
        case State::AwaitingResume:
            if (!retain(bytes)) {
                return fail(fatal(Status::ContentTooLarge));
            }
            return ParseProgress::Paused;
        case State::Closed:
            if (!bytes.empty()) {
                return fail(fatal(Status::BadRequest));
            }
            return ParseProgress::NeedMore;
        case State::Failed:
            break;
        }
        return std::unexpected(failure_);
    }

    ParseResult resume() noexcept {
        switch (state_) {
        case State::Parsing:
            return ParseProgress::NeedMore;
        case State::AwaitingReset:
            return ParseProgress::Paused;
        case State::AwaitingResume:
            state_ = State::Parsing;
            return execute(unparsed_, /*from_tail=*/true);
        case State::Closed:
            if (!unparsed_.empty()) {
                return fail(fatal(Status::BadRequest));
            }
            return ParseProgress::NeedMore;
        case State::Failed:
            break;
        }
        return std::unexpected(failure_);
    }

    void reset_for_next_request() noexcept {
        if (state_ != State::AwaitingReset) {
            return;
        }
        target_size_ = 0;
        header_bytes_used_ = 0;
        header_count_ = 0;
        in_name_ = false;
        content_length_ = 0;
        rejection_.reset();
        llhttp_reset(&parser_);
        // llhttp_reset() also forgets that the connection is closing, which llhttp would
        // otherwise enforce by failing on any byte after a non-keep-alive request.
        state_ = keep_alive_ ? State::AwaitingResume : State::Closed;
    }

private:
    static Impl& self(llhttp_t* parser) noexcept { return *static_cast<Impl*>(parser->data); }

    static const llhttp_settings_t& settings() noexcept {
        static const llhttp_settings_t callbacks = [] {
            llhttp_settings_t s{};
            llhttp_settings_init(&s);
            s.on_url = [](llhttp_t* p, const char* at, std::size_t n) noexcept {
                return self(p).on_url({at, n});
            };
            s.on_header_field = [](llhttp_t* p, const char* at, std::size_t n) noexcept {
                return self(p).on_header_field({at, n});
            };
            s.on_header_field_complete = [](llhttp_t* p) noexcept {
                return self(p).on_header_field_complete();
            };
            s.on_header_value = [](llhttp_t* p, const char* at, std::size_t n) noexcept {
                return self(p).on_header_value({at, n});
            };
            s.on_header_value_complete = [](llhttp_t* p) noexcept {
                return self(p).on_header_value_complete();
            };
            s.on_headers_complete = [](llhttp_t* p) noexcept {
                return self(p).on_headers_complete();
            };
            s.on_body = [](llhttp_t* p, const char* at, std::size_t n) noexcept {
                return self(p).on_body(std::as_bytes(std::span{at, n}));
            };
            s.on_message_complete = [](llhttp_t* p) noexcept {
                return self(p).on_message_complete();
            };
            return s;
        }();
        return callbacks;
    }

    ParseResult execute(std::span<const std::byte> input, bool from_tail) noexcept {
        const char* const begin = input.empty() ? &kNoInput : as_chars(input);
        const llhttp_errno_t err = llhttp_execute(&parser_, begin, input.size());
        if (err == HPE_OK) {
            if (from_tail) {
                unparsed_.clear();
            }
            return ParseProgress::NeedMore;
        }
        if (err != HPE_PAUSED) {
            return fail(error_for(err));
        }
        // A pause stops llhttp right behind the byte whose callback asked for it.
        const auto consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - begin);
        if (from_tail) {
            unparsed_.erase(unparsed_.begin(),
                            unparsed_.begin() + static_cast<std::ptrdiff_t>(consumed));
        } else if (!retain(input.subspan(consumed))) {
            return fail(fatal(Status::ContentTooLarge));
        }
        state_ = State::AwaitingReset;
        if (rejection_) {
            return std::unexpected(*rejection_);
        }
        return ParseProgress::MessageComplete;
    }

    [[nodiscard]] ParseError error_for(llhttp_errno_t err) const noexcept {
        // Our own callbacks fail with HPE_USER or HPE_CB_*, having recorded why.
        if (rejection_) {
            return *rejection_;
        }
        if (err == HPE_INVALID_VERSION) {
            return fatal(Status::HttpVersionNotSupported);
        }
        return fatal(Status::BadRequest);
    }

    ParseResult fail(ParseError error) noexcept {
        state_ = State::Failed;
        failure_ = error;
        unparsed_.clear();
        return std::unexpected(error);
    }

    bool retain(std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > kMaxRetainedBytes - unparsed_.size()) {
            return false;
        }
        unparsed_.insert(unparsed_.end(), bytes.begin(), bytes.end());
        return true;
    }

    int reject(Status status) noexcept {
        rejection_ = fatal(status);
        return -1;
    }

    int on_url(std::string_view fragment) noexcept {
        if (!append(target_, target_size_, fragment)) {
            return reject(Status::RequestHeaderFieldsTooLarge);
        }
        return 0;
    }

    int on_header_field(std::string_view fragment) noexcept {
        if (!in_name_) {
            if (header_count_ == kMaxHeaderCount) {
                return reject(Status::RequestHeaderFieldsTooLarge);
            }
            in_name_ = true;
            name_begin_ = header_bytes_used_;
        }
        return append_header_bytes(fragment);
    }

    int on_header_field_complete() noexcept {
        in_name_ = false;
        value_begin_ = header_bytes_used_;
        return 0;
    }

    int on_header_value(std::string_view fragment) noexcept {
        return append_header_bytes(fragment);
    }

    int append_header_bytes(std::string_view fragment) noexcept {
        if (!append(header_bytes_, header_bytes_used_, fragment)) {
            return reject(Status::RequestHeaderFieldsTooLarge);
        }
        return 0;
    }

    int on_header_value_complete() noexcept {
        const std::string_view stored{header_bytes_.data(), header_bytes_used_};
        const HeaderField field{
            .name = stored.substr(name_begin_, value_begin_ - name_begin_),
            .value = trim_ows(stored.substr(value_begin_)),
        };
        std::span{headers_}[header_count_++] = field;
        if (detail::iequals(field.name, "content-length")) {
            const auto length = parse_content_length(field.value);
            if (!length) {
                return reject(Status::BadRequest);
            }
            if (*length > kMaxContentLength) {
                return reject(Status::ContentTooLarge);
            }
            content_length_ = *length;
        }
        return 0;
    }

    int on_headers_complete() noexcept {
        // llhttp also accepts 0.9 (including a request line with no version at all) and 2.0.
        if (llhttp_get_http_major(&parser_) != 1 || llhttp_get_http_minor(&parser_) > 1) {
            return reject(Status::HttpVersionNotSupported);
        }
        keep_alive_ = llhttp_should_keep_alive(&parser_) != 0;
        const RequestHead head{
            .method = to_method(llhttp_get_method(&parser_)),
            .target = {target_.data(), target_size_},
            .version_minor = llhttp_get_http_minor(&parser_),
            .content_length = content_length_,
            .keep_alive = keep_alive_,
            .headers = std::span{headers_}.first(header_count_),
        };
        const std::optional<Status> rejected = sink_.on_head(head).rejection();
        if (!rejected) {
            return 0;
        }
        // Only a bodiless request on a persistent connection ends at a known boundary.
        const bool must_close = !keep_alive_ || content_length_ > 0;
        rejection_ = ParseError{.status = *rejected, .must_close = must_close};
        return must_close ? -1 : HPE_PAUSED;
    }

    int on_body(std::span<const std::byte> fragment) noexcept {
        sink_.on_body(fragment);
        return 0;
    }

    // Pausing here lets the connection finish this request, a durable write and its response,
    // before the next pipelined request reaches the sink.
    int on_message_complete() noexcept {
        sink_.on_message_complete();
        return HPE_PAUSED;
    }

    llhttp_t parser_{};
    IRequestSink& sink_;
    State state_ = State::Parsing;
    ParseError failure_ = fatal(Status::BadRequest);
    // Set by a callback that refused the request; cleared only by reset_for_next_request().
    std::optional<ParseError> rejection_;
    std::vector<std::byte> unparsed_;

    std::array<char, kMaxTargetBytes> target_{};
    std::size_t target_size_ = 0;
    std::array<char, kMaxHeaderBytes> header_bytes_{};
    std::size_t header_bytes_used_ = 0;
    std::array<HeaderField, kMaxHeaderCount> headers_{};
    std::size_t header_count_ = 0;
    std::size_t name_begin_ = 0;
    std::size_t value_begin_ = 0;
    bool in_name_ = false;
    std::uint64_t content_length_ = 0;
    bool keep_alive_ = true;
};

RequestParser::RequestParser(IRequestSink& sink) : impl_(std::make_unique<Impl>(sink)) {}

RequestParser::~RequestParser() = default;

ParseResult RequestParser::feed(std::span<const std::byte> bytes) noexcept {
    return impl_->feed(bytes);
}

ParseResult RequestParser::resume() noexcept {
    return impl_->resume();
}

void RequestParser::reset_for_next_request() noexcept {
    impl_->reset_for_next_request();
}

} // namespace http
