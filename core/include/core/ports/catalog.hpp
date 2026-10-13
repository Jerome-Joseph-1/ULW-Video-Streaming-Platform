#pragma once

#include "core/models/ids.hpp"
#include "core/models/storage_key.hpp"
#include "core/models/upload.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace core::ports {

enum class CatalogError : std::uint8_t {
    NotFound,
    // Another request holds the upload, or a row changed under an optimistic update.
    Conflict,
    // The database is unreachable or refused the statement; the request may be retried.
    Unavailable,
    // A stored row violates a domain invariant.
    Corrupt,
    // The caller may not make this change: a visibility naming a room they are not a member of.
    Forbidden,
};

[[nodiscard]] std::string_view to_string(CatalogError e) noexcept;

template <class T> using CatalogResult = std::expected<T, CatalogError>;

// Called exactly once, on the reactor thread, never from inside the call that was given it.
// Whatever it captures must outlive the call.
template <class T> using CatalogCallback = std::move_only_function<void(CatalogResult<T>) noexcept>;

// A create's Idempotency-Key (ADR-0108): the owner's key, and what the request asked for, so
// that a retry asking for something else under the same key is told apart from a retry.
struct UploadRequestKey {
    std::string key;
    std::string fingerprint;
};

struct NewUpload {
    VideoRecord video;
    UploadRecord upload;
    // Adapter state for the object store's ingest, persisted verbatim.
    std::string backend_ref;
    StorageKey object_key;
    std::optional<UploadRequestKey> request;
};

// The upload an owner's Idempotency-Key already names, as its create answered it.
struct KeyedUpload {
    VideoId video;
    UploadId upload;
    std::uint64_t chunk_size = 0;
    std::string fingerprint;
};

struct StoredUpload {
    UploadRecord upload;
    std::string backend_ref;
    StorageKey object_key;
};

// One grant of a claim on an upload. Every grant a catalog makes has a token of its own, and a
// release names the grant it gives back: a holder whose claim was lost and granted again, to it
// or to anyone else, cannot release the newer grant with the older token.
struct ClaimToken {
    std::uint64_t value = 0;

    friend bool operator==(const ClaimToken&, const ClaimToken&) = default;
};

struct ClaimedUpload {
    StoredUpload stored;
    ClaimToken token;
};

// A video as one viewer's request finds it: the row, and what the viewer is to it (ADR-0097).
struct VideoView {
    VideoRecord video;
    ViewerFacts viewer;
};

// One user the operator's backend granted a video to.
struct VideoGrant {
    UserId user;
    WallTime granted_at;
};

// A video's grants in user id order (bytewise), from after a cursor.
struct GrantPage {
    std::vector<VideoGrant> grants;
    // More follow the last one here.
    bool more = false;
};

// Where a page of one user's videos resumes (ADR-0100): after the video with this id, created at
// this instant (Unix microseconds, the database's own precision), in newest-first order.
struct VideoCursor {
    std::int64_t created_at_us = 0;
    VideoId id;
};

// One video of a user's listing, and when it was created.
struct ListedVideo {
    VideoRecord video;
    std::int64_t created_at_us = 0;
};

// A user's videos, newest first (created_at, then id, both descending), deleted ones left out.
struct VideoPage {
    std::vector<ListedVideo> videos;
    // More follow the last one here.
    bool more = false;
};

// Upload and video metadata as the gateway sees it. Every call is asynchronous: the gateway's
// reactor thread issues it and continues.
class IUploadCatalog {
public:
    virtual ~IUploadCatalog() = default;

    // The video and its upload, together. Idempotent for the same ids (a statement retried
    // after its answer was lost). With a request key the owner has used already for another
    // upload, Conflict and nothing is created (ADR-0108).
    virtual void create_upload(NewUpload upload, CatalogCallback<void> done) = 0;
    virtual void find_upload(const UploadId& id, CatalogCallback<StoredUpload> done) = 0;
    // The upload `owner`'s Idempotency-Key names; NotFound when none does, or when the upload is
    // gone (its video purged).
    virtual void find_keyed_upload(const UserId& owner, const std::string& key,
                                   CatalogCallback<KeyedUpload> done) = 0;

    // Exclusive right to append to one upload, across every gateway process. Refused with
    // Conflict while someone else holds it; released by release_upload or when the holding
    // process's database session ends, so a killed gateway frees its uploads at once. Refused
    // with NotFound, before any claim is taken, when `owner` does not own the upload: knowing
    // an upload's id must not be enough to lock its owner out.
    virtual void claim_upload(const UploadId& id, const UserId& owner,
                              CatalogCallback<ClaimedUpload> done) = 0;
    // Gives back the grant `token` names. A token that no longer names the upload's current
    // grant (released already, lost with its session, or replaced by a later claim) releases
    // nothing.
    virtual void release_upload(const UploadId& id, ClaimToken token) noexcept = 0;

    // Records a durable offset reached by the holder of the grant `token` names, and moves the
    // video from init to uploading on the first one. Offsets never move backwards. Refused with
    // Conflict when `token` is not the upload's current grant: an append that outlived its claim
    // records nothing under a later one.
    virtual void record_progress(const UploadId& id, ClaimToken token, const VideoId& video,
                                 std::uint64_t durable_offset, CatalogCallback<void> done) = 0;

    // One transaction: the upload completes, the video moves to processing and a transcode
    // job is queued. Idempotent: a repeat finds the work done and succeeds without a second job.
    // Answers the video's state as that transaction saw it: processing after the commit that
    // did the work, and whatever the worker has made of the video since on a repeat.
    virtual void commit_upload(const UploadId& id, const VideoId& video,
                               const std::string& request_id, CatalogCallback<VideoState> done) = 0;
    // Cancels an upload (ADR-0106): it is aborted, so no later append or commit goes through, and
    // its video, which nothing can commit now, is deleted as delete_video deletes one, staged
    // parts queued for the reaper. Idempotent: an upload aborted already succeeds, and its video
    // goes if an earlier abort left it. NotFound for no such upload; Conflict once it completed.
    virtual void abort_upload(const UploadId& id, CatalogCallback<void> done) = 0;

    // Who may see a video (ADR-0097). Every read a viewer's request makes goes through
    // find_video_for and core::access_of; nothing is cached, so a member taken off the room's
    // list, or a grant revoked, loses the video at their next request.

    // The video and what `viewer` is to it, in one read: whether chat_members lists them in
    // the room the video is shared with, and whether they hold a grant of it.
    virtual void find_video_for(const VideoId& id, const UserId& viewer,
                                CatalogCallback<VideoView> done) = 0;
    // NotFound unless `owner` owns a video by that id, exactly as for one that does not exist;
    // without an owner (the operator's backend, ADR-0100), unless a video by that id exists.
    // Forbidden when `visibility` names a room chat_members does not list the video's owner in.
    // Answers the video as it now stands.
    virtual void set_visibility(const VideoId& id, const std::optional<UserId>& owner,
                                const Visibility& visibility,
                                CatalogCallback<VideoRecord> done) = 0;
    // Deletes a video for good, in any state (ADR-0100, ADR-0106): an upload still open for it is
    // aborted, so no later append or commit can bring it back; from this call on every read
    // finds no such video, its grants are gone, a transcode job still queued for it is
    // cancelled, and its stored objects and staged parts are queued for the reaper, which
    // removes them and then the row. With `owner`, only a video of theirs; without (the operator's
    // backend), any. Idempotent: a video deleted already, and not removed by the reaper yet,
    // succeeds again. NotFound when no video by that id exists (or is not `owner`'s, or the reaper
    // has removed it).
    virtual void delete_video(const VideoId& id, const std::optional<UserId>& owner,
                              CatalogCallback<void> done) = 0;
    // At most `limit` of `owner`'s videos that are not deleted, newest first, from after
    // `after` (from the newest without one), every state included. A user with none gets an
    // empty page.
    virtual void list_videos(const UserId& owner, std::optional<VideoCursor> after,
                             std::size_t limit, CatalogCallback<VideoPage> done) = 0;
    // Both idempotent: a grant held already, or a revoke of one not held, succeeds. NotFound
    // when no video has that id.
    virtual void grant_access(const VideoId& id, const UserId& user,
                              CatalogCallback<void> done) = 0;
    virtual void revoke_access(const VideoId& id, const UserId& user,
                               CatalogCallback<void> done) = 0;
    // At most `limit` grants, of users whose ids sort after `after` bytewise (from the first
    // without one). NotFound when no video has that id.
    virtual void list_grants(const VideoId& id, std::optional<UserId> after, std::size_t limit,
                             CatalogCallback<GrantPage> done) = 0;
};

} // namespace core::ports
