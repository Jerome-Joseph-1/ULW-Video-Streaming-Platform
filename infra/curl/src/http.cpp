#include "infra/curl/http.hpp"

#include "exchange.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace infra::curl {

namespace {

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b,
                              [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view kBlank = " \t\r\n";
    const std::size_t first = s.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(kBlank) - first + 1);
}

class SpanSource final : public IBodySource {
public:
    explicit SpanSource(std::span<const std::byte> bytes) noexcept : rest_(bytes) {}

    std::size_t read_body(std::span<std::byte> out) noexcept override {
        const std::size_t n = std::min(out.size(), rest_.size());
        std::ranges::copy(rest_.first(n), out.begin());
        rest_ = rest_.subspan(n);
        return n;
    }

private:
    std::span<const std::byte> rest_;
};

class UploadSource final : public IBodySource {
public:
    explicit UploadSource(IUploadSource& source) noexcept : source_(source) {}

    std::size_t read_body(std::span<std::byte> out) noexcept override {
        return source_.read_upload(out);
    }

private:
    IUploadSource& source_;
};

Result run_blocking(const Request& request, std::uint64_t upload_length, IBodySource* source,
                    IDownloadSink* sink, CURL* reuse = nullptr) {
    auto exchange = detail::Exchange::create(request, upload_length, source,
                                             detail::Exchange::Mode::Blocking, sink, reuse);
    if (!exchange) {
        return std::unexpected(std::move(exchange.error()));
    }
    return (*exchange)->finish(curl_easy_perform((*exchange)->easy()));
}

} // namespace

std::string_view to_string(Method method) noexcept {
    switch (method) {
    case Method::Get:
        return "GET";
    case Method::Head:
        return "HEAD";
    case Method::Put:
        return "PUT";
    case Method::Post:
        return "POST";
    case Method::Patch:
        return "PATCH";
    case Method::Delete:
        return "DELETE";
    }
    return "GET";
}

std::optional<std::string_view> Response::header(std::string_view name) const noexcept {
    std::string_view rest = headers;
    while (!rest.empty()) {
        const std::size_t eol = rest.find('\n');
        const std::string_view line = rest.substr(0, eol);
        rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 1);
        const std::size_t colon = line.find(':');
        if (colon == name.size() && iequals(line.substr(0, colon), name)) {
            return trim(line.substr(colon + 1));
        }
    }
    return std::nullopt;
}

Result perform(const Request& request, std::span<const std::byte> body) {
    SpanSource source(body);
    return run_blocking(request, body.size(), &source, nullptr);
}

Result perform_download(const Request& request, IDownloadSink& sink) {
    return run_blocking(request, 0, nullptr, &sink);
}

Result perform_upload(const Request& request, std::uint64_t length, IUploadSource& source) {
    UploadSource adapter(source);
    return run_blocking(request, length, &adapter, nullptr);
}

Session::~Session() {
    if (easy_ != nullptr) {
        curl_easy_cleanup(easy_);
    }
}

void* Session::handle() noexcept {
    if (easy_ == nullptr && detail::global_init()) {
        easy_ = curl_easy_init();
    }
    return easy_;
}

Result Session::perform(const Request& request, std::span<const std::byte> body) {
    SpanSource source(body);
    CURL* const easy = handle();
    if (easy == nullptr) {
        return std::unexpected(Failure{.kind = FailureKind::Local, .detail = "curl init failed"});
    }
    return run_blocking(request, body.size(), &source, nullptr, easy);
}

Result Session::perform_upload(const Request& request, std::uint64_t length,
                               IUploadSource& source) {
    UploadSource adapter(source);
    CURL* const easy = handle();
    if (easy == nullptr) {
        return std::unexpected(Failure{.kind = FailureKind::Local, .detail = "curl init failed"});
    }
    return run_blocking(request, length, &adapter, nullptr, easy);
}

} // namespace infra::curl
