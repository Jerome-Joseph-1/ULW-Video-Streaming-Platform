#include "infra/catalog/memory_catalog.hpp"

#include <algorithm>
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

void MemoryCatalog::create_upload(core::ports::NewUpload upload, CatalogCallback<void> done) {
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
    const auto it = uploads_.find(id);
    core::ports::CatalogResult<StoredUpload> result =
        it == uploads_.end() ? std::unexpected(CatalogError::NotFound)
                             : core::ports::CatalogResult<StoredUpload>(it->second);
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::claim_upload(const core::UploadId& id, const core::UserId& owner,
                                 CatalogCallback<StoredUpload> done) {
    const auto it = uploads_.find(id);
    core::ports::CatalogResult<StoredUpload> result = std::unexpected(CatalogError::NotFound);
    if (it != uploads_.end() && it->second.upload.owner == owner) {
        if (claimed_.insert(id).second) {
            result = it->second;
        } else {
            result = std::unexpected(CatalogError::Conflict);
        }
    }
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

void MemoryCatalog::release_upload(const core::UploadId& id) noexcept {
    claimed_.erase(id);
}

void MemoryCatalog::record_progress(const core::UploadId& id, const core::VideoId& video,
                                    std::uint64_t durable_offset, CatalogCallback<void> done) {
    const auto it = uploads_.find(id);
    core::ports::CatalogResult<void> result{};
    if (it == uploads_.end()) {
        result = std::unexpected(CatalogError::NotFound);
    } else if (!claimed_.contains(id)) {
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
                                  const std::string& request_id, CatalogCallback<void> done) {
    core::ports::CatalogResult<void> result{};
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
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::abort_upload(const core::UploadId& id, CatalogCallback<void> done) {
    core::ports::CatalogResult<void> result{};
    if (const auto it = uploads_.find(id); it == uploads_.end()) {
        result = std::unexpected(CatalogError::NotFound);
    } else if (it->second.upload.state == core::UploadState::Active) {
        it->second.upload.state = core::UploadState::Aborted;
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryCatalog::find_video(const core::VideoId& id, CatalogCallback<core::VideoRecord> done) {
    const auto it = videos_.find(id);
    core::ports::CatalogResult<core::VideoRecord> result =
        it == videos_.end() ? std::unexpected(CatalogError::NotFound)
                            : core::ports::CatalogResult<core::VideoRecord>(it->second);
    defer([done = std::move(done), result = std::move(result)]() mutable noexcept {
        done(std::move(result));
    });
}

} // namespace infra::catalog
