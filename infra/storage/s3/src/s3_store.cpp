#include "infra/storage/s3_store.hpp"

#include "infra/curl/http.hpp"
#include "infra/s3util/url.hpp"
#include "infra/s3util/xml.hpp"

#include "control.hpp"
#include "endpoint.hpp"
#include "failure.hpp"
#include "multipart.hpp"
#include "part_session.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace infra::storage {

namespace {

using core::ports::IngestId;
using core::ports::StorageError;
using s3::Control;
using s3::Failed;
using s3::kMaxDocumentBytes;

// S3's largest ListParts page.
constexpr std::string_view kPartsPerPage = "1000";

// An id this store could have issued. It comes back from the caller's database, and a zero
// part size would divide by zero below.
bool plausible(const IngestId& id, const s3util::S3Profile& profile) noexcept {
    return !id.backend_ref.empty() && id.total_bytes > 0 &&
           id.chunk_size >= profile.min_part_bytes && id.chunk_size <= profile.max_part_bytes &&
           id.chunk_size > 0;
}

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const char* const end = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(std::to_address(text.begin()), end, value);
    if (text.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::uint64_t part_count(const IngestId& id) noexcept {
    return (id.total_bytes + id.chunk_size - 1) / id.chunk_size;
}

// How long part `number` (1-based) must be for the object to assemble to total_bytes.
std::uint64_t part_length(const IngestId& id, std::uint64_t number) noexcept {
    const std::uint64_t start = (number - 1) * id.chunk_size;
    return std::min(id.chunk_size, id.total_bytes - start);
}

// The leading parts S3 holds in full; the durable offset is exactly these. A part with the
// wrong size cannot be part of this object and ends the run like a missing one.
std::uint64_t whole_parts(const std::vector<s3util::UploadedPart>& parts,
                          const IngestId& id) noexcept {
    std::uint64_t n = 0;
    for (const auto& part : parts) {
        if (n == part_count(id) || part.part_number != n + 1 ||
            part.size != part_length(id, n + 1)) {
            break;
        }
        ++n;
    }
    return n;
}

// One listing request, retried, and its page parsed.
template <typename Page, typename Parse>
std::expected<Page, StorageError> fetch_page(const Control& control,
                                             const s3util::RequestTarget& target, Parse parse) {
    return control.retrying<Page>([&]() -> std::expected<Page, Failed> {
        auto response = control.send(curl::Method::Get, target, {}, {}, kMaxDocumentBytes);
        if (!response) {
            return std::unexpected(response.error());
        }
        auto parsed = parse(response->body);
        if (!parsed) {
            return std::unexpected(s3::failed(parsed.error()));
        }
        return std::move(*parsed);
    });
}

std::expected<std::vector<s3util::UploadedPart>, StorageError>
list_parts(const Control& control, const s3util::Bucket& bucket, const IngestId& id) {
    std::vector<s3util::UploadedPart> parts;
    std::optional<std::uint32_t> marker;
    while (true) {
        std::vector<s3util::QueryParam> query{
            {.name = "uploadId", .value = id.backend_ref},
            {.name = "max-parts", .value = std::string(kPartsPerPage)}};
        if (marker) {
            query.push_back({.name = "part-number-marker", .value = std::to_string(*marker)});
        }
        auto page = fetch_page<s3util::ListPartsResult>(
            control, bucket.object(id.key, std::move(query)), s3util::parse_list_parts);
        if (!page) {
            return std::unexpected(page.error());
        }
        std::ranges::move(page->parts, std::back_inserter(parts));
        if (!page->is_truncated) {
            break;
        }
        // A marker that does not move forward would page forever.
        const auto next = page->next_part_number_marker;
        if (!next || (marker && *next <= *marker)) {
            return std::unexpected(StorageError::Permanent);
        }
        marker = next;
    }
    std::ranges::sort(parts, {}, &s3util::UploadedPart::part_number);
    return parts;
}

std::expected<std::uint64_t, StorageError>
object_size(const Control& control, const s3util::Bucket& bucket, const core::StorageKey& key) {
    const auto target = bucket.object(key);
    return control.retrying<std::uint64_t>([&]() -> std::expected<std::uint64_t, Failed> {
        auto response = control.send(curl::Method::Head, target, {}, {}, 0);
        if (!response) {
            return std::unexpected(response.error());
        }
        const auto length = response->header("content-length");
        const auto size = length ? parse_u64(*length) : std::nullopt;
        if (!size) {
            return std::unexpected(Failed{.error = StorageError::Transient, .retry_after = {}});
        }
        return *size;
    });
}

// S3 no longer knows the upload: either a completion (ours, retried, or a racing commit's)
// consumed it, or it was aborted. Only the object itself can tell which.
std::expected<void, StorageError> completed(const Control& control, const s3util::Bucket& bucket,
                                            const IngestId& id) {
    const auto size = object_size(control, bucket, id.key);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size != id.total_bytes) {
        return std::unexpected(StorageError::NotFound);
    }
    return {};
}

} // namespace

std::expected<std::unique_ptr<S3Store>, S3ConfigError>
S3Store::create(Deps deps, const S3StoreOptions& options) {
    if (options.part_size < deps.profile.min_part_bytes ||
        options.part_size > deps.profile.max_part_bytes) {
        return std::unexpected(S3ConfigError::PartSizeOutOfRange);
    }
    // RetryPolicy throws on these; a configuration mistake is reported, not thrown.
    if (options.retry.base <= core::Millis::zero() || options.retry.cap < options.retry.base) {
        return std::unexpected(S3ConfigError::InvalidRetryPolicy);
    }
    auto bucket = s3util::Bucket::make(deps.profile, deps.bucket);
    if (!bucket) {
        return std::unexpected(S3ConfigError::InvalidBucket);
    }
    auto endpoint = std::make_unique<s3::Endpoint>(std::move(*bucket), deps.profile,
                                                   deps.credentials, deps.clock);
    auto pages = std::make_unique<s3::PageCount>();
    auto control = std::make_unique<Control>(*endpoint, options.retry, deps.random, *pages);
    return std::make_unique<S3Store>(Token{}, std::move(deps), options.part_size, std::move(pages),
                                     std::move(endpoint), std::move(control));
}

S3Store::S3Store(Token /*token*/, Deps deps, std::uint64_t part_size,
                 std::unique_ptr<s3::PageCount> pages, std::unique_ptr<s3::Endpoint> endpoint,
                 std::unique_ptr<s3::Control> control)
    : deps_(std::move(deps)), part_size_(part_size), pages_(std::move(pages)),
      endpoint_(std::move(endpoint)), control_(std::move(control)) {}

S3Store::~S3Store() = default;

std::uint64_t S3Store::paging_errors() const noexcept {
    return pages_->value();
}

std::expected<IngestId, StorageError> S3Store::create(const core::StorageKey& key,
                                                      std::uint64_t total_bytes,
                                                      const core::ContentType& type) {
    if (total_bytes == 0 || total_bytes > deps_.profile.max_parts * part_size_) {
        return std::unexpected(StorageError::Permanent);
    }
    auto upload_id = s3::initiate_upload(*control_, endpoint_->bucket(), key, type);
    if (!upload_id) {
        return std::unexpected(upload_id.error());
    }
    return IngestId{.key = key,
                    .backend_ref = std::move(*upload_id),
                    .total_bytes = total_bytes,
                    .chunk_size = part_size_};
}

std::expected<std::unique_ptr<core::ports::IIngestSession>, StorageError>
S3Store::open(const IngestId& id, std::uint64_t offset, core::ports::IIngestObserver& observer) {
    if (!plausible(id, deps_.profile)) {
        return std::unexpected(StorageError::NotFound);
    }
    // Parts are whole or absent, so only a part boundary, or the end, is a place to resume.
    if (offset > id.total_bytes || (offset % id.chunk_size != 0 && offset != id.total_bytes)) {
        return std::unexpected(StorageError::PreconditionFailed);
    }
    // Whether the parts before `offset` really exist would take a ListParts, which cannot run
    // on the reactor thread; a gap shows up at commit, which refuses to complete around it.
    return std::make_unique<s3::PartSession>(s3::SessionDeps{.reactor = deps_.reactor,
                                                             .multi = deps_.multi,
                                                             .endpoint = *endpoint_,
                                                             .pages = *pages_},
                                             id, offset, observer);
}

std::expected<std::uint64_t, StorageError> S3Store::durable_offset(const IngestId& id) {
    if (!plausible(id, deps_.profile)) {
        return std::unexpected(StorageError::NotFound);
    }
    const auto parts = list_parts(*control_, endpoint_->bucket(), id);
    if (!parts) {
        if (parts.error() != StorageError::NotFound) {
            return std::unexpected(parts.error());
        }
        return completed(*control_, endpoint_->bucket(), id).transform([&] {
            return id.total_bytes;
        });
    }
    return std::min(whole_parts(*parts, id) * id.chunk_size, id.total_bytes);
}

std::expected<void, StorageError> S3Store::commit(const IngestId& id) {
    if (!plausible(id, deps_.profile)) {
        return std::unexpected(StorageError::NotFound);
    }
    const s3util::Bucket& bucket = endpoint_->bucket();
    const auto parts = list_parts(*control_, bucket, id);
    if (!parts) {
        if (parts.error() != StorageError::NotFound) {
            return std::unexpected(parts.error());
        }
        return completed(*control_, bucket, id);
    }
    const std::uint64_t needed = part_count(id);
    if (whole_parts(*parts, id) != needed) {
        return std::unexpected(StorageError::PreconditionFailed);
    }
    // The ETags come from the listing just made, never from part responses held in memory:
    // a part re-sent by a later session replaces the earlier one and its ETag.
    std::vector<s3util::CompletedPart> completion;
    completion.reserve(needed);
    for (std::uint64_t i = 0; i < needed; ++i) {
        const auto& part = (*parts)[i];
        completion.push_back({.part_number = part.part_number, .etag = part.etag});
    }
    const auto done = s3::complete_upload(*control_, bucket, id.key, id.backend_ref, completion);
    if (!done && done.error() == StorageError::NotFound) {
        // A retry of a completion whose response was lost finds NoSuchUpload.
        return completed(*control_, bucket, id);
    }
    return done;
}

void S3Store::discard(const IngestId& id) noexcept {
    if (!plausible(id, deps_.profile)) {
        return;
    }
    // Gone already counts as done, which is all discard promises; any other failure leaves
    // the upload for the reaper.
    [[maybe_unused]] const auto aborted =
        s3::abort_upload(*control_, endpoint_->bucket(), id.key, id.backend_ref);
}

std::expected<core::ports::ReadGrant, StorageError> S3Store::grant_read(const core::StorageKey& key,
                                                                        core::Seconds ttl) {
    const core::Seconds granted = std::min(ttl, deps_.profile.max_presign_ttl);
    auto url = endpoint_->presign_get(endpoint_->bucket().object(key), granted);
    if (!url) {
        return std::unexpected(StorageError::Permanent);
    }
    return core::ports::ReadGrant{.kind = core::ports::ReadGrant::Kind::RedirectUrl,
                                  .value = std::move(*url),
                                  .ttl = granted};
}

std::expected<std::vector<std::byte>, StorageError>
S3Store::fetch_small(const core::StorageKey& key, std::size_t max) {
    const auto target = endpoint_->bucket().object(key);
    // A body over `max` fails the exchange rather than arriving cut short, and that failure
    // is Permanent, so it is not fetched again.
    return control_->retrying<std::vector<std::byte>>(
        [&]() -> std::expected<std::vector<std::byte>, Failed> {
            auto response = control_->send(curl::Method::Get, target, {}, {}, max);
            if (!response) {
                return std::unexpected(response.error());
            }
            const auto bytes = std::as_bytes(std::span(response->body));
            return std::vector<std::byte>(bytes.begin(), bytes.end());
        });
}

std::expected<void, StorageError> S3Store::put(const core::StorageKey& key,
                                               std::span<const std::byte> bytes) {
    const auto target = endpoint_->bucket().object(key);
    return control_->retrying<void>([&]() -> std::expected<void, Failed> {
        auto response = control_->send(curl::Method::Put, target, {}, bytes, 0);
        if (!response) {
            return std::unexpected(response.error());
        }
        return {};
    });
}

std::expected<void, StorageError> S3Store::remove(const core::StorageKey& key) {
    const auto target = endpoint_->bucket().object(key);
    return control_->retrying<void>([&]() -> std::expected<void, Failed> {
        auto response = control_->send(curl::Method::Delete, target, {}, {}, 0);
        // S3 answers 204 for a key that never existed; other implementations say 404.
        if (!response && response.error().error != StorageError::NotFound) {
            return std::unexpected(response.error());
        }
        return {};
    });
}

std::expected<std::vector<core::StorageKey>, StorageError> S3Store::list(std::string_view prefix) {
    std::vector<core::StorageKey> keys;
    std::optional<std::string> token;
    while (true) {
        std::vector<s3util::QueryParam> query{{.name = "list-type", .value = "2"},
                                              {.name = "prefix", .value = std::string(prefix)}};
        if (token) {
            query.push_back({.name = "continuation-token", .value = *token});
        }
        auto page = fetch_page<s3util::ListObjectsResult>(
            *control_, endpoint_->bucket().root(std::move(query)), s3util::parse_list_objects_v2);
        if (!page) {
            return std::unexpected(page.error());
        }
        for (const std::string& name : page->keys) {
            // Keys this system did not write, and could not address, are not ours to report.
            if (auto key = core::StorageKey::parse(name)) {
                keys.push_back(std::move(*key));
            }
        }
        if (!page->is_truncated) {
            return keys;
        }
        // A token that does not change would page forever.
        if (!page->next_continuation_token || page->next_continuation_token == token) {
            return std::unexpected(StorageError::Permanent);
        }
        token = std::move(page->next_continuation_token);
    }
}

std::expected<std::size_t, StorageError> S3Store::reap_abandoned(core::WallTime older_than) {
    std::size_t reaped = 0;
    std::optional<std::string> key_marker;
    std::optional<std::string> upload_marker;
    while (true) {
        std::vector<s3util::QueryParam> query{{.name = "uploads", .value = ""}};
        if (key_marker && upload_marker) {
            query.push_back({.name = "key-marker", .value = *key_marker});
            query.push_back({.name = "upload-id-marker", .value = *upload_marker});
        }
        auto page = fetch_page<s3util::ListMultipartUploadsResult>(
            *control_, endpoint_->bucket().root(std::move(query)),
            s3util::parse_list_multipart_uploads);
        if (!page) {
            return std::unexpected(page.error());
        }
        for (const auto& upload : page->uploads) {
            const auto key = core::StorageKey::parse(upload.key);
            if (upload.initiated >= older_than || !key) {
                continue;
            }
            if (s3::abort_upload(*control_, endpoint_->bucket(), *key, upload.upload_id)) {
                ++reaped;
            }
        }
        if (!page->is_truncated) {
            return reaped;
        }
        if (page->next_key_marker == key_marker && page->next_upload_id_marker == upload_marker) {
            return std::unexpected(StorageError::Permanent);
        }
        key_marker = std::move(page->next_key_marker);
        upload_marker = std::move(page->next_upload_id_marker);
    }
}

} // namespace infra::storage
