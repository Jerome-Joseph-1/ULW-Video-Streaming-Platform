#pragma once

#include "core/ports/catalog.hpp"
#include "core/ports/views.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace infra::postgres {

struct CatalogConfig {
    std::string conninfo;
    // 8..16. A catalog call holds a connection for a few sub-millisecond round trips, so 8
    // carry thousands of calls a second; 16 keeps two gateways, the workers and a migrator far
    // below Postgres' default max_connections of 100.
    std::size_t connections = 8;
    // TCP, TLS and SCRAM take a handful of round trips, milliseconds on the private network;
    // 5 s trips only when the server or the path is gone.
    core::Millis connect_timeout{5000};
    // Catalog statements touch single rows by primary key. One that has not answered in 5 s is
    // stuck behind a lock or a dead server, and a 503 the client retries beats holding on.
    core::Millis request_timeout{5000};
};

// IUploadCatalog on Postgres, driven by the reactor: no call ever blocks the loop.
//
// Upload claims are session-level advisory locks, all held on one dedicated connection, so the
// server drops them the moment this process's session ends: a killed gateway frees its uploads
// at once. The flip side: if that connection is lost, every claim is lost with it. Claims in
// flight then fail with Unavailable, and record_progress under a lost claim fails with
// Conflict; the holder has to claim again, and may find another gateway holds the upload.
//
// View events go out on the same pool as catalog calls, one INSERT per batch.
//
// The offload pool resolves host names and must be stopped before this is destroyed. Calls
// still outstanding at destruction are dropped without their callbacks.
class PgUploadCatalog final : public core::ports::IUploadCatalog, public core::ports::IViewLog {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    // Refuses a connection string that does not parse, and one that names a `service` or no
    // host: libpq would then look the host up itself, blocking the loop.
    [[nodiscard]] static std::expected<std::unique_ptr<PgUploadCatalog>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const CatalogConfig& config);

    // Only create() can make the token.
    PgUploadCatalog(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgUploadCatalog() override;
    PgUploadCatalog(const PgUploadCatalog&) = delete;
    PgUploadCatalog& operator=(const PgUploadCatalog&) = delete;
    PgUploadCatalog(PgUploadCatalog&&) = delete;
    PgUploadCatalog& operator=(PgUploadCatalog&&) = delete;

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
    void find_video(const core::VideoId& id,
                    core::ports::CatalogCallback<core::VideoRecord> done) override;
    void record_views(std::vector<core::ports::ViewEvent> batch,
                      core::ports::CatalogCallback<void> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
