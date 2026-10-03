#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::curl {

enum class Method : std::uint8_t { Get, Head, Put, Post, Patch, Delete };

[[nodiscard]] std::string_view to_string(Method method) noexcept;

struct Request {
    Method method = Method::Get;
    std::string url;
    // Complete "name: value" lines, sent as given.
    std::vector<std::string> headers;
    // Largest 2xx body kept; a longer one fails the exchange with BodyTooLarge. Error bodies
    // have a fixed bound of their own, so a small limit never hides why a request failed.
    std::size_t max_body = 0;
    // The whole exchange, from the moment the transfer is added; zero leaves it unbounded.
    // libcurl only enforces it once the transfer has a connection: one queued behind
    // MAX_TOTAL_CONNECTIONS waits unbounded and, if it waited too long, fails as soon as it
    // leaves the queue.
    std::chrono::milliseconds timeout{0};
    // A PEM file of the authorities a https peer's certificate must chain to, in place of the
    // system's: a Kubernetes API server's own cluster CA. Empty uses the system's. The
    // initializer lets designated initializers that predate it leave it out.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::string ca_file{};
};

struct Response {
    int status = 0;
    // The final response's header block as received, status line included.
    std::string headers;
    std::string body;

    // Case-insensitive; the value comes back trimmed. The first occurrence wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
};

enum class FailureKind : std::uint8_t {
    Resolve,
    Connect,
    Timeout,
    // The connection broke or the peer sent something that is not HTTP.
    Network,
    // The peer's certificate was refused; retrying cannot change that.
    Tls,
    BodyTooLarge,
    // Nothing reached the network: a malformed URL, an allocation failure, a refused option.
    Local,
};

// An exchange that ended without a complete response. A response is never a failure,
// whatever its status: CURLE_OK says nothing about 2xx.
struct Failure {
    FailureKind kind = FailureKind::Local;
    // libcurl's own explanation, for logs.
    std::string detail;
};

using Result = std::expected<Response, Failure>;

// Blocks for the whole exchange, so never on a reactor thread. Safe from many threads at
// once: every call has a handle and a connection of its own. `body` is sent with PUT, POST and
// PATCH and must be empty otherwise.
[[nodiscard]] Result perform(const Request& request, std::span<const std::byte> body = {});

// Feeds a blocking streamed upload. Nothing can wake a blocked transfer, so returning 0 before
// the declared length is a read failure and aborts the exchange.
class IUploadSource {
public:
    virtual ~IUploadSource() = default;
    virtual std::size_t read_upload(std::span<std::byte> out) noexcept = 0;
};

// Takes a 2xx body as it arrives; false aborts the exchange with FailureKind::Local.
class IDownloadSink {
public:
    virtual ~IDownloadSink() = default;
    [[nodiscard]] virtual bool write_download(std::span<const std::byte> bytes) noexcept = 0;
};

// perform(), with a 2xx body of any size handed to `sink` instead of kept in Response::body.
// An error body is still kept there, bounded, so the caller can tell why.
[[nodiscard]] Result perform_download(const Request& request, IDownloadSink& sink);
// perform() for a PUT or POST of exactly `length` bytes pulled from `source`.
[[nodiscard]] Result perform_upload(const Request& request, std::uint64_t length,
                                    IUploadSource& source);

} // namespace infra::curl
