#include "infra/catalog/memory_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace infra::catalog {

using core::ports::CatalogCallback;
using core::ports::CatalogError;
using core::ports::StoredUpload;

MemoryCatalog::MemoryCatalog(net::IReactor& reactor) : reactor_(reactor) {}

MemoryCatalog::~MemoryCatalog() {
    reactor_.cancel_timer(timer_);
}

void MemoryCatalog::defer(std::move_only_function<void() noexcept> fn) {
    pending_.push_back(std::move(fn));
    if (timer_ == net::TimerId{}) {
        timer_ = reactor_.arm_timer(core::Millis{0}, *this);
    }
}

void MemoryCatalog::on_timeout() noexcept {
    timer_ = {};
    // Callbacks may issue further calls; those land in a fresh batch for the next iteration.
    std::vector<std::move_only_function<void() noexcept>> batch;
    batch.swap(pending_);
    for (auto& fn : batch) {
        fn();
    }
}

template <class T> bool MemoryCatalog::refused(CatalogCallback<T>& done) {
    if (!calls_error_) {
        return false;
    }
    defer([done = std::move(done), error = *calls_error_]() mutable noexcept {
        done(std::unexpected(error));
    });
    return true;
}

void MemoryCatalog::create_upload(core::ports::NewUpload upload, CatalogCallback<void> done) {
    if (refused(done)) {
        return;
    }
    const core::UploadId id = upload.upload.id;
    const core::VideoId video = upload.video.id;
    if (uploads_.contains(id) || videos_.contains(video)) {
        defer([done = std::move(done)]() mutable noexcept {
            done(std::unexpected(CatalogError::Conflict));
        });
        return;
    }
    videos_.emplace(video, std::move(upload.video));
    uploads_.emplace(id, StoredUpload{.upload = upload.upload,
                                      .backend_ref = std::move(upload.backend_ref),
                                      .object_key = std::move(upload.object_key)});
    defer([done = std::move(done)]() mutable noexcept { done({}); });
}

void MemoryCatalog::find_upload(const core::UploadId& id, CatalogCallback<StoredUpload> done) {
    if (refused(done)) {
        return;
    }
    const auto it = uploads_.find(id);
    core::ports::CatalogResult<StoredUpload> result =
        it == uploads_.end() ? std::unexpected(CatalogError::NotFound)
                             : core::ports::CatalogResult<StoredUpload>(it->second);
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::claim_upload(const core::UploadId& id, const core::UserId& owner,
                                 CatalogCallback<core::ports::ClaimedUpload> done) {
    if (refused(done)) {
        return;
    }
    const auto it = uploads_.find(id);
    core::ports::CatalogResult<core::ports::ClaimedUpload> result =
        std::unexpected(CatalogError::NotFound);
    if (it != uploads_.end() && it->second.upload.owner == owner) {
        const core::ports::ClaimToken token{++last_token_};
        if (claimed_.try_emplace(id, token).second) {
            result = core::ports::ClaimedUpload{.stored = it->second, .token = token};
        } else {
            result = std::unexpected(CatalogError::Conflict);
        }
    }
    std::move_only_function<void() noexcept> answer =
        [done = std::move(done), result = std::move(result)]() mutable noexcept {
            done(std::move(result));
        };
    if (hold_claims_) {
        held_claims_.push_back(std::move(answer));
        return;
    }
    defer(std::move(answer));
}

void MemoryCatalog::hold_claims(bool held) {
    hold_claims_ = held;
    if (held) {
        return;
    }
    for (auto& answer : std::exchange(held_claims_, {})) {
        defer(std::move(answer));
    }
}

void MemoryCatalog::release_upload(const core::UploadId& id,
                                   core::ports::ClaimToken token) noexcept {
    if (const auto it = claimed_.find(id); it != claimed_.end() && it->second == token) {
        claimed_.erase(it);
    }
}

void MemoryCatalog::record_progress(const core::UploadId& id, core::ports::ClaimToken token,
                                    const core::VideoId& video, std::uint64_t durable_offset,
                                    CatalogCallback<void> done) {
    if (refused(done)) {
        return;
    }
    const auto it = uploads_.find(id);
    core::ports::CatalogResult<void> result{};
    if (it == uploads_.end()) {
        result = std::unexpected(CatalogError::NotFound);
    } else if (const auto claim = claimed_.find(id);
               claim == claimed_.end() || claim->second != token) {
        result = std::unexpected(CatalogError::Conflict);
    } else if (it->second.upload.state == core::UploadState::Active) {
        auto& offset = it->second.upload.durable_offset;
        offset = std::max(offset, durable_offset);
        if (const auto v = videos_.find(video);
            v != videos_.end() && v->second.state == core::VideoState::Init) {
            v->second.state = core::VideoState::Uploading;
            ++v->second.version;
        }
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::commit_upload(const core::UploadId& id, const core::VideoId& video,
                                  const std::string& request_id,
                                  CatalogCallback<core::VideoState> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<core::VideoState> result;
    const auto it = uploads_.find(id);
    const auto v = videos_.find(video);
    if (it == uploads_.end() || v == videos_.end()) {
        result = std::unexpected(CatalogError::NotFound);
    } else if (it->second.upload.state == core::UploadState::Aborted) {
        result = std::unexpected(CatalogError::Conflict);
    } else {
        it->second.upload.state = core::UploadState::Completed;
        if (v->second.state == core::VideoState::Init ||
            v->second.state == core::VideoState::Uploading) {
            v->second.state = core::VideoState::Processing;
            ++v->second.version;
        }
        const bool live =
            std::ranges::any_of(jobs_, [&](const Job& j) { return j.video == video; });
        if (!live) {
            jobs_.push_back(
                Job{.video = video, .source_key = it->second.object_key, .request_id = request_id});
        }
        result = v->second.state;
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::abort_upload(const core::UploadId& id, CatalogCallback<void> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<void> result{};
    if (const auto it = uploads_.find(id); it == uploads_.end()) {
        result = std::unexpected(CatalogError::NotFound);
    } else if (it->second.upload.state == core::UploadState::Active) {
        it->second.upload.state = core::UploadState::Aborted;
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::find_video(const core::VideoId& id, CatalogCallback<core::VideoRecord> done) {
    if (refused(done)) {
        return;
    }
    const auto it = videos_.find(id);
    core::ports::CatalogResult<core::VideoRecord> result =
        it == videos_.end() ? std::unexpected(CatalogError::NotFound)
                            : core::ports::CatalogResult<core::VideoRecord>(it->second);
    if (find_video_error_) {
        result = std::unexpected(*find_video_error_);
    }
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::find_video_for(const core::VideoId& id, const core::UserId& viewer,
                                   CatalogCallback<core::ports::VideoView> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<core::ports::VideoView> result =
        std::unexpected(CatalogError::NotFound);
    if (const auto it = videos_.find(id); it != videos_.end()) {
        const core::VideoRecord& video = it->second;
        const std::optional<core::RoomId>& room = video.visibility.room_id();
        const auto granted = grants_.find(id);
        result = core::ports::VideoView{
            .video = video,
            .viewer = {.room_member =
                           room && members_.contains({*room, std::string(viewer.view())}),
                       .granted = granted != grants_.end() &&
                                  granted->second.contains(std::string(viewer.view()))}};
    }
    if (find_video_error_) {
        result = std::unexpected(*find_video_error_);
    }
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::set_visibility(const core::VideoId& id, const core::UserId& owner,
                                   const core::Visibility& visibility,
                                   CatalogCallback<core::VideoRecord> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<core::VideoRecord> result = std::unexpected(CatalogError::NotFound);
    if (const auto it = videos_.find(id); it != videos_.end() && it->second.owner == owner) {
        const std::optional<core::RoomId>& room = visibility.room_id();
        if (room && !members_.contains({*room, std::string(owner.view())})) {
            result = std::unexpected(CatalogError::Forbidden);
        } else {
            it->second.visibility = visibility;
            result = it->second;
        }
    }
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::grant_access(const core::VideoId& id, const core::UserId& user,
                                 CatalogCallback<void> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<void> result{};
    if (!videos_.contains(id)) {
        result = std::unexpected(CatalogError::NotFound);
    } else {
        grants_[id].try_emplace(
            std::string(user.view()),
            core::ports::VideoGrant{.user = user, .granted_at = std::chrono::system_clock::now()});
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::revoke_access(const core::VideoId& id, const core::UserId& user,
                                  CatalogCallback<void> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<void> result{};
    if (!videos_.contains(id)) {
        result = std::unexpected(CatalogError::NotFound);
    } else if (const auto it = grants_.find(id); it != grants_.end()) {
        it->second.erase(std::string(user.view()));
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::list_grants(const core::VideoId& id, std::optional<core::UserId> after,
                                std::size_t limit, CatalogCallback<core::ports::GrantPage> done) {
    if (refused(done)) {
        return;
    }
    core::ports::CatalogResult<core::ports::GrantPage> result =
        std::unexpected(CatalogError::NotFound);
    if (videos_.contains(id)) {
        core::ports::GrantPage page;
        if (const auto it = grants_.find(id); it != grants_.end()) {
            auto from =
                after ? it->second.upper_bound(std::string(after->view())) : it->second.begin();
            for (; from != it->second.end(); ++from) {
                if (page.grants.size() == limit) {
                    page.more = true;
                    break;
                }
                page.grants.push_back(from->second);
            }
        }
        result = std::move(page);
    }
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::add_member(const core::RoomId& room, const core::UserId& user) {
    members_.emplace(room, std::string(user.view()));
}

void MemoryCatalog::remove_member(const core::RoomId& room, const core::UserId& user) {
    members_.erase({room, std::string(user.view())});
}

void MemoryCatalog::record_views(std::vector<core::ports::ViewEvent> batch,
                                 CatalogCallback<void> done) {
    core::ports::CatalogResult<void> result{};
    if (views_error_) {
        result = std::unexpected(*views_error_);
    } else {
        views_.insert(views_.end(), batch.begin(), batch.end());
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::put_video(core::VideoRecord video) {
    const core::VideoId id = video.id;
    videos_.insert_or_assign(id, std::move(video));
}

} // namespace infra::catalog
