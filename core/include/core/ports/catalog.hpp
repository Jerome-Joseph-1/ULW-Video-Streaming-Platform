#pragma once

#include "core/models/ids.hpp"
#include "core/models/storage_key.hpp"
#include "core/models/upload.hpp"
#include "core/models/video.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <string>

namespace core::ports {

enum class CatalogError : std::uint8_t {
    NotFound,
    // Another request holds the upload, or a row changed under an optimistic update.
    Conflict,
    // The database is unreachable or refused the statement; the request may be retried.
    Unavailable,
    // A stored row violates a domain invariant.
    Corrupt,
};

[[nodiscard]] std::string_view to_string(CatalogError e) noexcept;

template <class T> using CatalogResult = std::expected<T, CatalogError>;

// Called exactly once, on the reactor thread, never from inside the call that was given it.
// Whatever it captures must outlive the call.
template <class T> using CatalogCallback = std::move_only_function<void(CatalogResult<T>) noexcept>;

struct NewUpload {
    VideoRecord video;
    UploadRecord upload;
    // Adapter state for the object store's ingest, persisted verbatim.
    std::string backend_ref;
    StorageKey object_key;
};

struct StoredUpload {
    UploadRecord upload;
    std::string backend_ref;
    StorageKey object_key;
};

// Upload and video metadata as the gateway sees it. Every call is asynchronous: the gateway's
// reactor thread issues it and continues.
class IUploadCatalog {
public:
    virtual ~IUploadCatalog() = default;

    virtual void create_upload(NewUpload upload, CatalogCallback<void> done) = 0;
    virtual void find_upload(const UploadId& id, CatalogCallback<StoredUpload> done) = 0;

    // Exclusive right to append to one upload, across every gateway process. Refused with
    // Conflict while someone else holds it; released by release_upload or when the holding
    // process's database session ends, so a killed gateway frees its uploads at once. Refused
    // with NotFound, before any claim is taken, when `owner` does not own the upload: knowing
    // an upload's id must not be enough to lock its owner out.
    virtual void claim_upload(const UploadId& id, const UserId& owner,
                              CatalogCallback<StoredUpload> done) = 0;
    virtual void release_upload(const UploadId& id) noexcept = 0;

    // Records a durable offset reached by the holder of the claim, and moves the video from
    // init to uploading on the first one. Offsets never move backwards.
    virtual void record_progress(const UploadId& id, const VideoId& video,
                                 std::uint64_t durable_offset, CatalogCallback<void> done) = 0;

    // One transaction: the upload completes, the video moves to processing and a transcode
    // job is queued. Idempotent: a repeat finds the work done and succeeds without a second job.
    virtual void commit_upload(const UploadId& id, const VideoId& video,
                               const std::string& request_id, CatalogCallback<void> done) = 0;
    virtual void abort_upload(const UploadId& id, CatalogCallback<void> done) = 0;

    virtual void find_video(const VideoId& id, CatalogCallback<VideoRecord> done) = 0;
};

} // namespace core::ports
