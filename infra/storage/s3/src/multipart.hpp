#pragma once

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "core/ports/storage.hpp"
#include "infra/s3util/url.hpp"
#include "infra/s3util/xml.hpp"

#include "control.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>

namespace infra::storage::s3 {

// The largest document S3 sends here is a 1,000-entry listing: at most 1,000 keys of 1,024
// bytes plus a few hundred bytes of markup each, about 1.4 MB. 4 MiB leaves room for keys
// that arrive entity-escaped.
inline constexpr std::size_t kMaxDocumentBytes = std::size_t{4} << 20U;

// Starts a multipart upload and returns its id. Not idempotent: a retry after a lost response
// leaves the first upload orphaned, for reap_abandoned() or the bucket's lifecycle rule.
[[nodiscard]] std::expected<std::string, core::ports::StorageError>
initiate_upload(const Control& control, const s3util::Bucket& bucket, const core::StorageKey& key,
                const core::ContentType& type);

// Assembles the object from `parts`. The status line goes out before S3 has finished
// assembling, so a late failure arrives as an <Error> inside a 200 and the body is always read.
[[nodiscard]] std::expected<void, core::ports::StorageError>
complete_upload(const Control& control, const s3util::Bucket& bucket, const core::StorageKey& key,
                const std::string& upload_id, std::span<const s3util::CompletedPart> parts);

// Aborts one multipart upload; NotFound means it was already gone.
[[nodiscard]] std::expected<void, core::ports::StorageError>
abort_upload(const Control& control, const s3util::Bucket& bucket, const core::StorageKey& key,
             const std::string& upload_id);

} // namespace infra::storage::s3
