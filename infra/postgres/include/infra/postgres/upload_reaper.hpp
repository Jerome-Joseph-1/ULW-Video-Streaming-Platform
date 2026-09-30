#pragma once

#include "core/ports/unused_rooms.hpp"
#include "core/ports/upload_expiry.hpp"

#include <memory>
#include <string>

namespace infra::postgres {

// IUploadExpiry and IUnusedRooms on Postgres. One blocking session, opened on first use and
// replaced on the call after it broke.
class PgUploadReaper final : public core::ports::IUploadExpiry, public core::ports::IUnusedRooms {
public:
    explicit PgUploadReaper(std::string conninfo);
    ~PgUploadReaper() override;
    PgUploadReaper(const PgUploadReaper&) = delete;
    PgUploadReaper& operator=(const PgUploadReaper&) = delete;
    PgUploadReaper(PgUploadReaper&&) = delete;
    PgUploadReaper& operator=(PgUploadReaper&&) = delete;

    [[nodiscard]] std::expected<std::vector<core::ports::ExpiredUpload>, core::ports::CatalogError>
    expire(core::WallTime now, std::size_t limit) override;
    [[nodiscard]] std::expected<std::size_t, core::ports::CatalogError>
    forget_unused(core::WallTime recorded_before, std::size_t limit) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
