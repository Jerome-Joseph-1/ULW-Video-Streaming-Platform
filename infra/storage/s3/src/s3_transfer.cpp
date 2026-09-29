#include "infra/storage/s3_transfer.hpp"

#include "core/util/parse.hpp"
#include "infra/curl/http.hpp"
#include "infra/s3util/crypto.hpp"
#include "infra/s3util/sigv4.hpp"
#include "os/unique_fd.hpp"

#include "control.hpp"
#include "endpoint.hpp"
#include "failure.hpp"
#include "multipart.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace infra::storage {

namespace {

using core::ports::StorageError;
using s3::Failed;

// Large enough that a read costs a syscall per 256 KiB, small enough to sit on the stack of
// the worker's one transfer thread.
constexpr std::size_t kFileBuffer = std::size_t{256} << 10U;

Failed local_failure() noexcept {
    return {.error = StorageError::Permanent, .retry_after = std::nullopt};
}

class FileSink final : public curl::IDownloadSink {
public:
    explicit FileSink(int fd) noexcept : fd_(fd) {}

    bool write_download(std::span<const std::byte> bytes) noexcept override {
        while (!bytes.empty()) {
            const ssize_t n = ::write(fd_, bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(n));
            written_ += static_cast<std::uint64_t>(n);
        }
        return true;
    }

    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }

private:
    int fd_;
    std::uint64_t written_ = 0;
};

// Reads by offset, so a retried upload starts over without seeking a shared descriptor.
class FileSource final : public curl::IUploadSource {
public:
    explicit FileSource(int fd) noexcept : fd_(fd) {}

    std::size_t read_upload(std::span<std::byte> out) noexcept override {
        while (true) {
            const ssize_t n = ::pread(fd_, out.data(), out.size(), offset_);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return 0;
            }
            offset_ += n;
            return static_cast<std::size_t>(n);
        }
    }

private:
    int fd_;
    off_t offset_ = 0;
};

std::expected<std::string, StorageError> file_sha256(int fd) {
    s3util::Sha256Stream hash;
    std::array<std::byte, kFileBuffer> buffer{};
    off_t offset = 0;
    while (true) {
        const ssize_t n = ::pread(fd, buffer.data(), buffer.size(), offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return std::unexpected(StorageError::Permanent);
        }
        if (n == 0) {
            return s3util::to_hex(hash.finish());
        }
        hash.update(std::span(buffer).first(static_cast<std::size_t>(n)));
        offset += n;
    }
}

// Parts go up one at a time as they fill; the ETags S3 answers with are all the state kept.
class S3ObjectStream final : public core::ports::IObjectStream {
public:
    S3ObjectStream(const s3::Control& control, const s3util::Bucket& bucket, core::StorageKey key,
                   std::string upload_id, std::uint32_t max_parts)
        : control_(control), bucket_(bucket), key_(std::move(key)),
          upload_id_(std::move(upload_id)), max_parts_(max_parts) {
        buffer_.reserve(S3Transfer::kStreamPart);
    }
    ~S3ObjectStream() override {
        if (!done_) {
            // An abort that fails leaves the upload to the bucket's lifecycle rule.
            [[maybe_unused]] const auto aborted =
                s3::abort_upload(control_, bucket_, key_, upload_id_);
        }
    }
    S3ObjectStream(const S3ObjectStream&) = delete;
    S3ObjectStream& operator=(const S3ObjectStream&) = delete;
    S3ObjectStream(S3ObjectStream&&) = delete;
    S3ObjectStream& operator=(S3ObjectStream&&) = delete;

    std::expected<void, StorageError> write(std::span<const std::byte> bytes) override {
        if (done_) {
            return std::unexpected(StorageError::Permanent);
        }
        while (!bytes.empty()) {
            const std::size_t take =
                std::min(bytes.size(), S3Transfer::kStreamPart - buffer_.size());
            buffer_.insert(buffer_.end(), bytes.begin(),
                           bytes.begin() + static_cast<std::ptrdiff_t>(take));
            bytes = bytes.subspan(take);
            if (buffer_.size() == S3Transfer::kStreamPart) {
                if (auto sent = send_part(); !sent) {
                    return sent;
                }
            }
        }
        return {};
    }

    std::expected<void, StorageError> commit() override {
        if (done_) {
            return {};
        }
        // Only the last part may be short, and an object needs one part even when empty.
        if (!buffer_.empty() || parts_.empty()) {
            if (auto sent = send_part(); !sent) {
                return sent;
            }
        }
        auto completed = s3::complete_upload(control_, bucket_, key_, upload_id_, parts_);
        done_ = completed.has_value();
        return completed;
    }

private:
    std::expected<void, StorageError> send_part() {
        if (parts_.size() == max_parts_) {
            return std::unexpected(StorageError::Permanent);
        }
        const auto number = static_cast<std::uint32_t>(parts_.size() + 1);
        const auto target =
            bucket_.object(key_, {{.name = "partNumber", .value = std::to_string(number)},
                                  {.name = "uploadId", .value = upload_id_}});
        auto etag = control_.retrying<std::string>([&]() -> std::expected<std::string, Failed> {
            auto response = control_.send(curl::Method::Put, target, {}, buffer_, 0);
            if (!response) {
                return std::unexpected(response.error());
            }
            const auto tag = response->header("etag");
            if (!tag) {
                return std::unexpected(Failed{.error = StorageError::Corrupt, .retry_after = {}});
            }
            return std::string(*tag);
        });
        if (!etag) {
            return std::unexpected(etag.error());
        }
        parts_.push_back({.part_number = number, .etag = std::move(*etag)});
        buffer_.clear();
        return {};
    }

    const s3::Control& control_;
    const s3util::Bucket& bucket_;
    core::StorageKey key_;
    std::string upload_id_;
    std::uint32_t max_parts_;
    std::vector<std::byte> buffer_;
    std::vector<s3util::CompletedPart> parts_;
    bool done_ = false;
};

std::optional<std::uint64_t> content_length(const curl::Response& response) {
    const auto header = response.header("content-length");
    return header ? core::parse_integer<std::uint64_t>(*header) : std::nullopt;
}

} // namespace

std::expected<std::unique_ptr<S3Transfer>, S3TransferConfigError>
S3Transfer::create(Deps deps, const s3util::RetryPolicy::Config& retry) {
    // RetryPolicy throws on these; a configuration mistake is reported, not thrown.
    if (retry.base <= core::Millis::zero() || retry.cap < retry.base) {
        return std::unexpected(S3TransferConfigError::InvalidRetryPolicy);
    }
    auto bucket = s3util::Bucket::make(deps.profile, deps.bucket);
    if (!bucket) {
        return std::unexpected(S3TransferConfigError::InvalidBucket);
    }
    auto endpoint = std::make_unique<s3::Endpoint>(std::move(*bucket), deps.profile,
                                                   deps.credentials, deps.clock);
    auto pages = std::make_unique<s3::PageCount>();
    auto control = std::make_unique<s3::Control>(*endpoint, retry, deps.random, *pages);
    return std::make_unique<S3Transfer>(Token{}, std::move(deps), std::move(pages),
                                        std::move(endpoint), std::move(control));
}

S3Transfer::S3Transfer(Token /*token*/, Deps deps, std::unique_ptr<s3::PageCount> pages,
                       std::unique_ptr<s3::Endpoint> endpoint, std::unique_ptr<s3::Control> control)
    : deps_(std::move(deps)), pages_(std::move(pages)), endpoint_(std::move(endpoint)),
      control_(std::move(control)) {}

std::uint64_t S3Transfer::paging_errors() const noexcept {
    return pages_->value();
}

S3Transfer::~S3Transfer() = default;

std::expected<std::uint64_t, StorageError> S3Transfer::size(const core::StorageKey& key) {
    const auto target = endpoint_->bucket().object(key);
    return control_->retrying<std::uint64_t>([&]() -> std::expected<std::uint64_t, Failed> {
        auto response = control_->send(curl::Method::Head, target, {}, {}, 0);
        if (!response) {
            return std::unexpected(response.error());
        }
        const auto length = content_length(*response);
        if (!length) {
            return std::unexpected(Failed{.error = StorageError::Corrupt, .retry_after = {}});
        }
        return *length;
    });
}

std::expected<std::uint64_t, StorageError>
S3Transfer::download(const core::StorageKey& key, const std::filesystem::path& destination) {
    const auto target = endpoint_->bucket().object(key);
    return control_->retrying<std::uint64_t>([&]() -> std::expected<std::uint64_t, Failed> {
        // Truncated per attempt: a retry after a broken transfer starts from the first byte.
        const os::UniqueFd fd(
            ::open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (!fd) {
            return std::unexpected(local_failure());
        }
        FileSink sink(fd.get());
        const auto request = endpoint_->sign(curl::Method::Get, target, {},
                                             s3util::kEmptyPayloadSha256, /*max_body=*/0);
        auto response = curl::perform_download(request, sink);
        if (!response) {
            return std::unexpected(s3::failed(response.error()));
        }
        if (!s3::is_success(*response)) {
            return std::unexpected(s3::failed(*response));
        }
        // libcurl already fails a body cut short of its Content-Length; this catches a
        // response that declared none.
        if (content_length(*response) != sink.written()) {
            return std::unexpected(Failed{.error = StorageError::Transient, .retry_after = {}});
        }
        return sink.written();
    });
}

std::expected<void, StorageError> S3Transfer::upload(const std::filesystem::path& source,
                                                     const core::StorageKey& key,
                                                     const core::ContentType& type) {
    return put(source, key, type, false);
}

std::expected<void, StorageError> S3Transfer::upload_new(const std::filesystem::path& source,
                                                         const core::StorageKey& key,
                                                         const core::ContentType& type) {
    if (!deps_.profile.supports_conditional_put) {
        return std::unexpected(StorageError::Permanent);
    }
    const auto put_result = put(source, key, type, true);
    if (!put_result && put_result.error() == StorageError::PreconditionFailed) {
        return std::unexpected(StorageError::AlreadyExists);
    }
    return put_result;
}

std::expected<std::unique_ptr<core::ports::IObjectStream>, StorageError>
S3Transfer::begin(const core::StorageKey& key, const core::ContentType& type) {
    auto upload_id = s3::initiate_upload(*control_, endpoint_->bucket(), key, type);
    if (!upload_id) {
        return std::unexpected(upload_id.error());
    }
    return std::make_unique<S3ObjectStream>(*control_, endpoint_->bucket(), key,
                                            std::move(*upload_id), deps_.profile.max_parts);
}

std::expected<void, StorageError> S3Transfer::put(const std::filesystem::path& source,
                                                  const core::StorageKey& key,
                                                  const core::ContentType& type, bool create_only) {
    // Never through a link: the worker uploads what a sandboxed ffmpeg wrote, and a link there
    // could name any file the worker can read.
    const os::UniqueFd fd(::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st {};
    if (!fd || ::fstat(fd.get(), &st) != 0) {
        return std::unexpected(StorageError::Permanent);
    }
    const auto length = static_cast<std::uint64_t>(st.st_size);
    // One PUT carries at most what one part may: 5 GiB on S3, R2 and MinIO alike. The worker
    // uploads segments and playlists, none within three orders of magnitude of that.
    if (length > deps_.profile.max_part_bytes) {
        return std::unexpected(StorageError::Permanent);
    }
    const auto hash = file_sha256(fd.get());
    if (!hash) {
        return std::unexpected(hash.error());
    }
    std::vector<s3util::Header> headers{
        {.name = "content-type", .value = std::string(type.view())}};
    if (create_only) {
        headers.push_back({.name = "if-none-match", .value = "*"});
    }
    const auto target = endpoint_->bucket().object(key);
    return control_->retrying<void>([&]() -> std::expected<void, Failed> {
        FileSource body(fd.get());
        const auto request = endpoint_->sign(curl::Method::Put, target, headers, *hash, 0);
        auto response = curl::perform_upload(request, length, body);
        if (!response) {
            return std::unexpected(s3::failed(response.error()));
        }
        if (!s3::is_success(*response)) {
            return std::unexpected(s3::failed(*response));
        }
        return {};
    });
}

} // namespace infra::storage
