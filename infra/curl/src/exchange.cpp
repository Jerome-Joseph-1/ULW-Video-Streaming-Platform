#include "exchange.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace infra::curl::detail {

namespace {

// A store that takes longer than this to accept a TCP connection is down as far as the
// caller is concerned; failing fast lets its retry policy decide.
constexpr long kConnectTimeoutMs = 3000;
// Less than a byte a second for a whole minute is a dead peer. The clock keeps running while
// an upload is paused for want of body bytes (libcurl 8.5 exempts only paused receives), so a
// client that stalls mid-body for that long loses the request as well.
constexpr long kLowSpeedBytes = 1;
constexpr long kLowSpeedSeconds = 60;
// S3 error documents are well under 1 KiB. 64 KiB keeps whatever explains a failure while
// still bounding what a broken peer can make us hold.
constexpr std::size_t kMaxErrorBody = std::size_t{64} << 10U;
// S3 sends under 1 KiB of response headers; 16 KiB is the common server-side limit.
constexpr std::size_t kMaxHeaderBytes = std::size_t{16} << 10U;

FailureKind classify(CURLcode code, bool oversize) noexcept {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return FailureKind::Resolve;
    case CURLE_COULDNT_CONNECT:
        return FailureKind::Connect;
    case CURLE_OPERATION_TIMEDOUT:
        return FailureKind::Timeout;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_ISSUER_ERROR:
        return FailureKind::Tls;
    case CURLE_WRITE_ERROR:
        return oversize ? FailureKind::BodyTooLarge : FailureKind::Local;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_OUT_OF_MEMORY:
    case CURLE_BAD_FUNCTION_ARGUMENT:
    case CURLE_FAILED_INIT:
    case CURLE_READ_ERROR:
    case CURLE_ABORTED_BY_CALLBACK:
        return FailureKind::Local;
    default:
        return FailureKind::Network;
    }
}

bool is_success(long status) noexcept {
    return status >= 200 && status <= 299;
}

} // namespace

bool global_init() noexcept {
    // A function-local static runs the initialiser exactly once even when the first calls
    // race; libcurl requires the init to finish before any handle exists.
    static const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ok;
}

Exchange::Exchange(Token /*token*/, std::size_t max_body, std::uint64_t upload_length,
                   IBodySource* source, Mode mode, IDownloadSink* sink) noexcept
    : max_body_(max_body), upload_left_(source == nullptr ? 0 : upload_length), source_(source),
      mode_(mode), sink_(sink) {}

std::expected<std::unique_ptr<Exchange>, Failure> Exchange::create(const Request& request,
                                                                   std::uint64_t upload_length,
                                                                   IBodySource* source, Mode mode,
                                                                   IDownloadSink* sink) {
    if (!global_init()) {
        return std::unexpected(Failure{.kind = FailureKind::Local, .detail = "curl init failed"});
    }
    auto exchange =
        std::make_unique<Exchange>(Token{}, request.max_body, upload_length, source, mode, sink);
    if (auto configured = exchange->configure(request); !configured) {
        return std::unexpected(std::move(configured.error()));
    }
    return exchange;
}

std::expected<void, Failure> Exchange::configure(const Request& request) {
    const auto local = [](std::string detail) {
        return std::unexpected(Failure{.kind = FailureKind::Local, .detail = std::move(detail)});
    };
    easy_.reset(curl_easy_init());
    if (!easy_) {
        return local("curl_easy_init failed");
    }
    const auto append = [this](const char* line) {
        curl_slist* const head = header_list_.release();
        curl_slist* const grown = curl_slist_append(head, line);
        header_list_.reset(grown == nullptr ? head : grown);
        return grown != nullptr;
    };
    for (const std::string& line : request.headers) {
        // A line break inside one header would let its value start another.
        if (line.find_first_of("\r\n") != std::string::npos) {
            return local("header line contains a line break");
        }
        if (!append(line.c_str())) {
            return local("curl_slist_append failed");
        }
    }
    // libcurl adds "Expect: 100-continue" to larger uploads and then holds the body back
    // until the server answers or a second passes. An empty value removes the header.
    if (!append("Expect:")) {
        return local("curl_slist_append failed");
    }

    CURL* const e = easy_.get();
    bool ok = true;
    const auto set = [&ok](CURLcode rc) { ok = ok && rc == CURLE_OK; };
    set(curl_easy_setopt(e, CURLOPT_URL, request.url.c_str()));
    set(curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https"));
    // TLS 1.3 at least, whatever the host's OpenSSL configuration allows: libcurl's own default
    // floor is TLS 1.0, and a distribution's openssl.cnf raises it to 1.2 at most. Every host
    // the services call over https (Askedin's JWKS, R2, an https MinIO) must speak TLS 1.3
    // (docs/integration/operations-contract.md): it drops TLS 1.2's static-RSA and CBC suites
    // and renegotiation, and encrypts the certificate. The maximum is TLS 1.3 by name, the
    // newest any TLS library offers, rather than CURL_SSLVERSION_MAX_DEFAULT, whose value is
    // TLS 1.0 shifted into the maximum's bits. The option is read as a long. libcurl 8.5 declares
    // the two values in separate enums, where or-ing them is a deprecated conversion, so each
    // becomes a long by implicit conversion first, which also holds whatever integer type another
    // release gives them, without a cast that could be useless there.
    constexpr long kTlsMin = CURL_SSLVERSION_TLSv1_3;
    constexpr long kTlsMax = CURL_SSLVERSION_MAX_TLSv1_3;
    constexpr long kTlsVersions = kTlsMin | kTlsMax;
    set(curl_easy_setopt(e, CURLOPT_SSLVERSION, kTlsVersions));
    // Otherwise libcurl swaps signal handlers around every call, which races between threads;
    // with it, sends use MSG_NOSIGNAL instead.
    set(curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L));
    // Removing a handle whose name lookup is still running would otherwise join the resolver
    // thread, holding the reactor for as long as getaddrinfo takes (libcurl 8.5,
    // Curl_resolver_kill). The same join sits on the connect-timeout path. With this set the
    // thread is left to finish on its own and frees what it holds when it does.
    set(curl_easy_setopt(e, CURLOPT_QUICK_EXIT, 1L));
    // A redirect would send a signed request to wherever the Location header points.
    set(curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L));
    // Over HTTP/2 every part to one host would share one connection and one flow-control
    // window, so a single stalled upload could slow all the others.
    // The option is read as a long. libcurl 8.5 declares the value in an enum and 8.14 (the
    // worker image's, docs/adr/0074) as a long, so a cast would be useless on one of them.
    constexpr long kHttp11 = CURL_HTTP_VERSION_1_1;
    set(curl_easy_setopt(e, CURLOPT_HTTP_VERSION, kHttp11));
    set(curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs));
    set(curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytes));
    set(curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, kLowSpeedSeconds));
    // The option is read through varargs as a long, whatever the duration's own type.
    const long timeout_ms = request.timeout.count();
    set(curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, timeout_ms));
    set(curl_easy_setopt(e, CURLOPT_ERRORBUFFER, error_.data()));
    set(curl_easy_setopt(e, CURLOPT_HTTPHEADER, header_list_.get()));
    set(curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, &Exchange::on_header));
    set(curl_easy_setopt(e, CURLOPT_HEADERDATA, this));
    set(curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &Exchange::on_body));
    set(curl_easy_setopt(e, CURLOPT_WRITEDATA, this));
    switch (request.method) {
    case Method::Get:
        set(curl_easy_setopt(e, CURLOPT_HTTPGET, 1L));
        break;
    case Method::Head:
        set(curl_easy_setopt(e, CURLOPT_NOBODY, 1L));
        break;
    case Method::Delete:
        set(curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, "DELETE"));
        break;
    case Method::Put:
        set(curl_easy_setopt(e, CURLOPT_UPLOAD, 1L));
        set(curl_easy_setopt(e, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(upload_left_)));
        set(curl_easy_setopt(e, CURLOPT_READFUNCTION, &Exchange::on_read));
        set(curl_easy_setopt(e, CURLOPT_READDATA, this));
        break;
    case Method::Post:
        set(curl_easy_setopt(e, CURLOPT_POST, 1L));
        set(curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(upload_left_)));
        set(curl_easy_setopt(e, CURLOPT_READFUNCTION, &Exchange::on_read));
        set(curl_easy_setopt(e, CURLOPT_READDATA, this));
        break;
    }
    if (!ok) {
        return local("curl option refused");
    }
    return {};
}

Result Exchange::finish(CURLcode code) {
    if (code == CURLE_OK) {
        long status = 0;
        static_cast<void>(curl_easy_getinfo(easy_.get(), CURLINFO_RESPONSE_CODE, &status));
        response_.status = static_cast<int>(status);
        return std::move(response_);
    }
    const std::string_view detail =
        error_[0] != '\0' ? std::string_view(error_.data()) : curl_easy_strerror(code);
    return std::unexpected(
        Failure{.kind = classify(code, oversize_), .detail = std::string(detail)});
}

void Exchange::resume_body() noexcept {
    if (!paused_) {
        return;
    }
    // Cleared first: libcurl may call on_read from inside curl_easy_pause, and a read that
    // finds nothing must be able to pause again.
    paused_ = false;
    // It can only fail for a handle with no connection, and a paused upload has one.
    static_cast<void>(curl_easy_pause(easy_.get(), CURLPAUSE_CONT));
}

std::size_t Exchange::on_header(char* data, std::size_t size, std::size_t count,
                                void* self) noexcept {
    auto& ex = *static_cast<Exchange*>(self);
    const std::size_t n = size * count;
    const std::string_view line(data, n);
    // Interim responses have header blocks of their own; only the final one is kept.
    if (line.starts_with("HTTP/")) {
        ex.response_.headers.clear();
    }
    if (ex.response_.headers.size() + n > kMaxHeaderBytes) {
        ex.oversize_ = true;
        return CURL_WRITEFUNC_ERROR;
    }
    ex.response_.headers.append(line);
    return n;
}

std::size_t Exchange::on_body(char* data, std::size_t size, std::size_t count,
                              void* self) noexcept {
    auto& ex = *static_cast<Exchange*>(self);
    const std::size_t n = size * count;
    long status = 0;
    static_cast<void>(curl_easy_getinfo(ex.easy_.get(), CURLINFO_RESPONSE_CODE, &status));
    if (ex.sink_ != nullptr && is_success(status)) {
        const auto bytes = std::as_bytes(std::span(data, n));
        return ex.sink_->write_download(bytes) ? n : CURL_WRITEFUNC_ERROR;
    }
    const std::size_t limit = is_success(status) ? ex.max_body_ : kMaxErrorBody;
    if (ex.response_.body.size() + n > limit) {
        ex.oversize_ = true;
        return CURL_WRITEFUNC_ERROR;
    }
    ex.response_.body.append(data, n);
    return n;
}

std::size_t Exchange::on_read(char* data, std::size_t size, std::size_t count,
                              void* self) noexcept {
    auto& ex = *static_cast<Exchange*>(self);
    const std::size_t room = std::min<std::uint64_t>(size * count, ex.upload_left_);
    if (room == 0) {
        // Only reachable once the declared length has gone out, where 0 is the true end.
        return 0;
    }
    const std::size_t n =
        std::min(room, ex.source_->read_body(std::span(reinterpret_cast<std::byte*>(data), room)));
    if (n == 0) {
        if (ex.mode_ == Mode::Blocking) {
            return CURL_READFUNC_ABORT;
        }
        // Returning 0 here would end the body short of its Content-Length and truncate it.
        ex.paused_ = true;
        return CURL_READFUNC_PAUSE;
    }
    ex.upload_left_ -= n;
    return n;
}

} // namespace infra::curl::detail
