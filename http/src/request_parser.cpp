#include "http/request_parser.hpp"

#include "http/method.hpp"
#include "http/request.hpp"
#include "http/status.hpp"

#include "ascii.hpp"

#include <algorithm>
#include <array>
#include <bitset>
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
#include <utility>
#include <vector>

namespace http {

namespace {

enum class State : std::uint8_t {
    Parsing,
    // The sink paused inside a body; llhttp is paused with it.
    PausedInBody,
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

// Fields the gateway reads with find_header(), which returns the first copy. A second copy is a
// second chance for something in front of this server to act on a different value than it does.
//   host           RFC 9112 3.2 answers more than one, or none on HTTP/1.1, with 400.
//   authorization  the bearer token.
//   cookie         the session token when there is no bearer; RFC 6265 5.4 allows one field.
//   upload-offset  where an upload chunk lands.
//   content-type   a singleton (RFC 9110 8.3) that says how a body is to be read.
// Content-Length is not listed because llhttp refuses a second one itself.
constexpr std::array<std::string_view, 5> kSingleValued{"host", "authorization", "cookie",
                                                        "upload-offset", "content-type"};
constexpr std::size_t kHostField = 0;
static_assert(kSingleValued[kHostField] == "host");

std::optional<std::size_t> single_valued_index(std::string_view name) noexcept {
    const auto* const it = std::ranges::find_if(
        kSingleValued, [name](std::string_view field) { return detail::iequals(field, name); });
    if (it == kSingleValued.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - kSingleValued.begin());
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
    explicit Impl(IRequestSink& sink) : sink_(sink) {
        llhttp_init(&parser_, HTTP_REQUEST, &settings());
        parser_.data = this;
        // Held bytes never exceed this, so holding more never allocates on the data path.
        unparsed_.reserve(kMaxRetainedBytes);
        // Strict llhttp fails "HTTP/1.2" with the error it also uses for "HTTP/1.1x" or a bare
        // LF, so a 505 could not be told from a 400. Leniently it parses any digit.digit and
        // on_headers_complete() refuses the ones that are not 1.0 or 1.1.
        llhttp_set_lenient_version(&parser_, 1);
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
        case State::PausedInBody:
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
        case State::PausedInBody:
            llhttp_resume(&parser_);
            state_ = State::Parsing;
            return execute(held(), /*from_tail=*/true);
        case State::AwaitingReset:
            return ParseProgress::Paused;
        case State::AwaitingResume:
            state_ = State::Parsing;
            return execute(held(), /*from_tail=*/true);
        case State::Closed:
            if (!held().empty()) {
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
        single_valued_seen_.reset();
        head_bytes_ = 0;
        headers_begun_ = false;
        head_complete_ = false;
        in_name_ = false;
        content_length_ = 0;
        transfer_coded_ = false;
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
        // Until the head is complete llhttp gets no more than the head budget has left, so the
        // bytes it skips without a callback run out where the budget does.
        const std::size_t first =
            head_complete_ ? input.size() : std::min(input.size(), kMaxHeadBytes - head_bytes_);
        llhttp_errno_t err = llhttp_execute(&parser_, begin, first);
        if (err == HPE_OK && !head_complete_) {
            head_bytes_ += first;
            if (head_bytes_ == kMaxHeadBytes) {
                return fail(fatal(headers_begun_ ? Status::RequestHeaderFieldsTooLarge
                                                 : Status::BadRequest));
            }
        }
        if (err == HPE_PAUSED && std::exchange(head_pause_, false)) {
            // The head ended inside the slice the budget allowed. The body runs on through the
            // whole input from here, so the sink gets the rest of a receive as one fragment:
            // cut at the slice, a sink pausing on the first part would leave the second held
            // here, and a receive would occupy both the sink's buffer and this one.
            llhttp_resume(&parser_);
            const char* const at = llhttp_get_error_pos(&parser_);
            err = llhttp_execute(&parser_, at, input.size() - static_cast<std::size_t>(at - begin));
        }
        if (err == HPE_OK) {
            if (from_tail) {
                forget_held();
            }
            return ParseProgress::NeedMore;
        }
        if (err != HPE_PAUSED) {
            return fail(error_for());
        }
        // A pause stops llhttp right behind the bytes whose callback asked for it; that callback
        // has also moved state_ to say why.
        const auto consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - begin);
        if (from_tail) {
            held_from_ += consumed;
        } else if (!retain(input.subspan(consumed))) {
            return fail(fatal(Status::ContentTooLarge));
        }
        if (state_ == State::PausedInBody) {
            return ParseProgress::Paused;
        }
        if (rejection_) {
            return std::unexpected(*rejection_);
        }
        return ParseProgress::MessageComplete;
    }

    [[nodiscard]] ParseError error_for() const noexcept {
        // Our own callbacks fail with HPE_USER or HPE_CB_*, having recorded why.
        if (rejection_) {
            return *rejection_;
        }
        return fatal(Status::BadRequest);
    }

    ParseResult fail(ParseError error) noexcept {
        state_ = State::Failed;
        failure_ = error;
        forget_held();
        return std::unexpected(error);
    }

    [[nodiscard]] std::span<const std::byte> held() const noexcept {
        return std::span{unparsed_}.subspan(held_from_);
    }

    void forget_held() noexcept {
        unparsed_.clear();
        held_from_ = 0;
    }

    bool retain(std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > kMaxRetainedBytes - held().size()) {
            return false;
        }
        // Parsing held bytes only moves held_from_, and the parsed prefix is dropped here rather
        // than after every request, so a burst of pipelined requests costs one pass, not one
        // shift of the remainder per request.
        unparsed_.erase(unparsed_.begin(),
                        unparsed_.begin() + static_cast<std::ptrdiff_t>(held_from_));
        held_from_ = 0;
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
        headers_begun_ = true;
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
        if (const auto index = single_valued_index(field.name)) {
            if (single_valued_seen_.test(*index)) {
                return reject(Status::BadRequest);
            }
            single_valued_seen_.set(*index);
        }
        if (detail::iequals(field.name, "content-length")) {
            const auto length = parse_content_length(field.value);
            if (!length) {
                return reject(Status::BadRequest);
            }
            if (*length > kMaxContentLength) {
                return reject(Status::ContentTooLarge);
            }
            content_length_ = *length;
        } else if (detail::iequals(field.name, "transfer-encoding")) {
            transfer_coded_ = true;
        }
        return 0;
    }

    int on_headers_complete() noexcept {
        head_complete_ = true;
        // Any digit.digit gets here, and so does a request line with no version at all, which
        // llhttp reads as 0.9.
        if (llhttp_get_http_major(&parser_) != 1 || llhttp_get_http_minor(&parser_) > 1) {
            return reject(Status::HttpVersionNotSupported);
        }
        if (llhttp_get_http_minor(&parser_) == 1 && !single_valued_seen_.test(kHostField)) {
            return reject(Status::BadRequest);
        }
        // After a CONNECT head llhttp treats the connection as a tunnel and skips whatever body
        // it declares, so those bytes would be parsed as the next request. Nothing here
        // tunnels, so the length does not matter.
        if (llhttp_get_method(&parser_) == HTTP_CONNECT) {
            return reject(Status::NotImplemented);
        }
        const Method method = to_method(llhttp_get_method(&parser_));
        // Content-Length is the only body framing accepted. A second one is a second chance to
        // disagree with a proxy about where the body ends, and nothing here needs streaming
        // bodies of unknown length. llhttp has already refused Transfer-Encoding alongside
        // Content-Length.
        if (transfer_coded_) {
            const bool takes_body =
                method == Method::Post || method == Method::Put || method == Method::Patch;
            return reject(takes_body ? Status::LengthRequired : Status::BadRequest);
        }
        keep_alive_ = llhttp_should_keep_alive(&parser_) != 0;
        const RequestHead head{
            .method = method,
            .target = {target_.data(), target_size_},
            .version_minor = llhttp_get_http_minor(&parser_),
            .content_length = content_length_,
            .keep_alive = keep_alive_,
            .headers = std::span{headers_}.first(header_count_),
        };
        const std::optional<Status> rejected = sink_.on_head(head).rejection();
        if (!rejected) {
            head_pause_ = true;
            return HPE_PAUSED;
        }
        // Only a bodiless request on a persistent connection ends at a known boundary.
        const bool must_close = !keep_alive_ || content_length_ > 0;
        rejection_ = ParseError{.status = *rejected, .must_close = must_close};
        if (must_close) {
            return -1;
        }
        state_ = State::AwaitingReset;
        return HPE_PAUSED;
    }

    // Returning HPE_PAUSED is how a callback pauses llhttp; llhttp_pause() must not be called
    // from inside one.
    int on_body(std::span<const std::byte> fragment) noexcept {
        // llhttp flushes an open body span at the end of every execute, so resuming with
        // nothing new to parse yields an empty one.
        if (fragment.empty()) {
            return 0;
        }
        switch (sink_.on_body(fragment)) {
        case BodyVerdict::Continue:
            return 0;
        case BodyVerdict::Pause:
            break;
        }
        state_ = State::PausedInBody;
        return HPE_PAUSED;
    }

    // Pausing here lets the connection finish this request, a durable write and its response,
    // before the next pipelined request reaches the sink.
    int on_message_complete() noexcept {
        sink_.on_message_complete();
        state_ = State::AwaitingReset;
        return HPE_PAUSED;
    }

    llhttp_t parser_{};
    IRequestSink& sink_;
    State state_ = State::Parsing;
    ParseError failure_ = fatal(Status::BadRequest);
    // Set by a callback that refused the request; cleared only by reset_for_next_request().
    std::optional<ParseError> rejection_;
    // Bytes fed while stopped; those before held_from_ have been parsed since.
    std::vector<std::byte> unparsed_;
    std::size_t held_from_ = 0;

    std::array<char, kMaxTargetBytes> target_{};
    std::size_t target_size_ = 0;
    std::array<char, kMaxHeaderBytes> header_bytes_{};
    std::size_t header_bytes_used_ = 0;
    std::array<HeaderField, kMaxHeaderCount> headers_{};
    std::size_t header_count_ = 0;
    std::bitset<kSingleValued.size()> single_valued_seen_;
    std::size_t head_bytes_ = 0;
    bool headers_begun_ = false;
    bool head_complete_ = false;
    // on_headers_complete paused llhttp only so that execute() can carry on past the head
    // budget; nobody outside sees that pause.
    bool head_pause_ = false;
    std::size_t name_begin_ = 0;
    std::size_t value_begin_ = 0;
    bool in_name_ = false;
    std::uint64_t content_length_ = 0;
    bool transfer_coded_ = false;
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
