#pragma once

#include "core/ports/catalog.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/views.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace infra::catalog {

// The catalog without a database, for tests and single-process development. Same contract as
// the durable catalog: results arrive on a later loop iteration, commit is one atomic step
// and queues at most one live job per video.
class MemoryCatalog final : public core::ports::IUploadCatalog,
                            public core::ports::IViewLog,
                            public net::ITimerHandler {
public:
    struct Job {
        core::VideoId video;
        core::StorageKey source_key;
        std::string request_id;
    };

    // `clock` stamps grants, as the database's now() does.
    MemoryCatalog(net::IReactor& reactor, const core::ports::IClock& clock);
    ~MemoryCatalog() override;
    MemoryCatalog(const MemoryCatalog&) = delete;
    MemoryCatalog& operator=(const MemoryCatalog&) = delete;

    void create_upload(core::ports::NewUpload upload,
                       core::ports::CatalogCallback<void> done) override;
    void find_upload(const core::UploadId& id,
                     core::ports::CatalogCallback<core::ports::StoredUpload> done) override;
    void claim_upload(const core::UploadId& id, const core::UserId& owner,
                      core::ports::CatalogCallback<core::ports::ClaimedUpload> done) override;
    void release_upload(const core::UploadId& id, core::ports::ClaimToken token) noexcept override;
    void record_progress(const core::UploadId& id, core::ports::ClaimToken token,
                         const core::VideoId& video, std::uint64_t durable_offset,
                         core::ports::CatalogCallback<void> done) override;
    void commit_upload(const core::UploadId& id, const core::VideoId& video,
                       const std::string& request_id,
                       core::ports::CatalogCallback<core::VideoState> done) override;
    void abort_upload(const core::UploadId& id, core::ports::CatalogCallback<void> done) override;
    void find_video_for(const core::VideoId& id, const core::UserId& viewer,
                        core::ports::CatalogCallback<core::ports::VideoView> done) override;
    void set_visibility(const core::VideoId& id, const std::optional<core::UserId>& owner,
                        const core::Visibility& visibility,
                        core::ports::CatalogCallback<core::VideoRecord> done) override;
    void delete_video(const core::VideoId& id, const std::optional<core::UserId>& owner,
                      core::ports::CatalogCallback<void> done) override;
    void list_videos(const core::UserId& owner, std::optional<core::ports::VideoCursor> after,
                     std::size_t limit,
                     core::ports::CatalogCallback<core::ports::VideoPage> done) override;
    void grant_access(const core::VideoId& id, const core::UserId& user,
                      core::ports::CatalogCallback<void> done) override;
    void revoke_access(const core::VideoId& id, const core::UserId& user,
                       core::ports::CatalogCallback<void> done) override;
    void list_grants(const core::VideoId& id, std::optional<core::UserId> after, std::size_t limit,
                     core::ports::CatalogCallback<core::ports::GrantPage> done) override;
    void record_views(std::vector<core::ports::ViewEvent> batch,
                      core::ports::CatalogCallback<void> done) override;

    void on_timeout() noexcept override;

    // The room member list chat keeps in chat_members, as find_video_for reads it.
    void add_member(const core::RoomId& room, const core::UserId& user);
    void remove_member(const core::RoomId& room, const core::UserId& user);

    // A video as the worker leaves it, for tests that begin after the upload.
    void put_video(core::VideoRecord video);
    // Fails every record_views from now on with `error`, or none with nullopt.
    void fail_views(std::optional<core::ports::CatalogError> error) noexcept {
        views_error_ = error;
    }
    // Fails every upload and video call from now on with `error`, or none with nullopt; the
    // catalog's state is left as it was.
    void fail_calls(std::optional<core::ports::CatalogError> error) noexcept {
        calls_error_ = error;
    }
    // Fails every find_video_for from now on with `error`, or none with nullopt.
    void fail_find_video(std::optional<core::ports::CatalogError> error) noexcept {
        find_video_error_ = error;
    }

    // While held, claim_upload takes or refuses the claim at once but its answer waits, as a
    // database's would in flight; hold_claims(false) sends every answer that waited.
    void hold_claims(bool held);
    [[nodiscard]] std::size_t held_claims() const noexcept { return held_claims_.size(); }

    [[nodiscard]] const std::vector<Job>& jobs() const noexcept { return jobs_; }
    // The videos deleted, in the order they were, as the reaper finds them in video_purges.
    [[nodiscard]] const std::vector<core::VideoId>& purges() const noexcept { return purges_; }
    [[nodiscard]] std::size_t claims() const noexcept { return claimed_.size(); }
    [[nodiscard]] const std::vector<core::ports::ViewEvent>& views() const noexcept {
        return views_;
    }

private:
    void defer(std::move_only_function<void() noexcept> fn);
    // True, and `done` answered with calls_error_ on a later iteration, when calls fail.
    template <class T> [[nodiscard]] bool refused(core::ports::CatalogCallback<T>& done);
    // The video by that id, unless it does not exist or has been deleted.
    [[nodiscard]] core::VideoRecord* live_video(const core::VideoId& id);
    [[nodiscard]] static std::optional<core::TranscodeProgress>
    progress_of(const core::VideoRecord& video);
    void stamp_created(const core::VideoId& id);

    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    bool hold_claims_ = false;
    std::vector<std::move_only_function<void() noexcept>> held_claims_;
    std::unordered_map<core::UploadId, core::ports::StoredUpload> uploads_;
    std::unordered_map<core::VideoId, core::VideoRecord> videos_;
    // When each video was created, in microseconds as the database keeps it.
    std::unordered_map<core::VideoId, std::int64_t> created_;
    // Deleted videos keep their rows until the reaper's purge, but no read finds them.
    std::set<core::VideoId> deleted_;
    std::vector<core::VideoId> purges_;
    std::set<std::pair<core::RoomId, std::string>> members_;
    // Each video's grants by user id, which a std::string orders bytewise as the database does.
    std::unordered_map<core::VideoId, std::map<std::string, core::ports::VideoGrant>> grants_;
    // Each claim held, by the token of its grant.
    std::unordered_map<core::UploadId, core::ports::ClaimToken> claimed_;
    std::uint64_t last_token_ = 0;
    std::vector<Job> jobs_;
    std::vector<core::ports::ViewEvent> views_;
    std::optional<core::ports::CatalogError> views_error_;
    std::optional<core::ports::CatalogError> calls_error_;
    std::optional<core::ports::CatalogError> find_video_error_;
};

} // namespace infra::catalog
