#pragma once

#include "core/ports/catalog.hpp"
#include "core/ports/views.hpp"
#include "net/reactor.hpp"

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
    void record_views(std::vector<core::ports::ViewEvent> batch,
                      core::ports::CatalogCallback<void> done) override;

    void on_timeout() noexcept override;

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

    [[nodiscard]] const std::vector<Job>& jobs() const noexcept { return jobs_; }
    [[nodiscard]] std::size_t claims() const noexcept { return claimed_.size(); }
    [[nodiscard]] const std::vector<core::ports::ViewEvent>& views() const noexcept {
        return views_;
    }

private:
    void defer(std::move_only_function<void() noexcept> fn);
    // True, and `done` answered with calls_error_ on a later iteration, when calls fail.
    template <class T> [[nodiscard]] bool refused(core::ports::CatalogCallback<T>& done);

    net::IReactor& reactor_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    std::unordered_map<core::UploadId, core::ports::StoredUpload> uploads_;
    std::unordered_map<core::VideoId, core::VideoRecord> videos_;
    std::unordered_set<core::UploadId> claimed_;
    std::vector<Job> jobs_;
    std::vector<core::ports::ViewEvent> views_;
    std::optional<core::ports::CatalogError> views_error_;
    std::optional<core::ports::CatalogError> calls_error_;
};

} // namespace infra::catalog
