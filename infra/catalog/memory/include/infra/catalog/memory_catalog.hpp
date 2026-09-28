#pragma once

#include "core/ports/catalog.hpp"
#include "net/reactor.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace infra::catalog {

// The catalog without a database, for tests and single-process development. Same contract as
// the durable catalog: results arrive on a later loop iteration, commit is one atomic step
// and queues at most one live job per video.
class MemoryCatalog final : public core::ports::IUploadCatalog, public net::ITimerHandler {
public:
    struct Job {
        core::VideoId video;
        core::StorageKey source_key;
        std::string request_id;
    };

    explicit MemoryCatalog(net::IReactor& reactor);
    ~MemoryCatalog() override;
    MemoryCatalog(const MemoryCatalog&) = delete;
    MemoryCatalog& operator=(const MemoryCatalog&) = delete;

    void create_upload(core::ports::NewUpload upload,
                       core::ports::CatalogCallback<void> done) override;
    void find_upload(const core::UploadId& id,
                     core::ports::CatalogCallback<core::ports::StoredUpload> done) override;
    void claim_upload(const core::UploadId& id, const core::UserId& owner,
                      core::ports::CatalogCallback<core::ports::StoredUpload> done) override;
    void release_upload(const core::UploadId& id) noexcept override;
    void record_progress(const core::UploadId& id, const core::VideoId& video,
                         std::uint64_t durable_offset,
                         core::ports::CatalogCallback<void> done) override;
    void commit_upload(const core::UploadId& id, const core::VideoId& video,
                       const std::string& request_id,
                       core::ports::CatalogCallback<void> done) override;
    void abort_upload(const core::UploadId& id, core::ports::CatalogCallback<void> done) override;
    void find_video(const core::VideoId& id,
                    core::ports::CatalogCallback<core::VideoRecord> done) override;

    void on_timeout() noexcept override;

    [[nodiscard]] const std::vector<Job>& jobs() const noexcept { return jobs_; }
    [[nodiscard]] std::size_t claims() const noexcept { return claimed_.size(); }

private:
    void defer(std::move_only_function<void() noexcept> fn);

    net::IReactor& reactor_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    std::unordered_map<core::UploadId, core::ports::StoredUpload> uploads_;
    std::unordered_map<core::VideoId, core::VideoRecord> videos_;
    std::unordered_set<core::UploadId> claimed_;
    std::vector<Job> jobs_;
};

} // namespace infra::catalog
