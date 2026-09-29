#pragma once

#include "core/ports/object_stream.hpp"
#include "core/ports/object_transfer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace infra::storage {

// IObjectTransfer over FsStore's layout (objects/<key> under the same root), so a worker in
// development reads what a gateway on the same root committed. Needs no reactor.
class FsTransfer final : public core::ports::IObjectTransfer, public core::ports::IObjectStreams {
public:
    explicit FsTransfer(const std::filesystem::path& root);

    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    size(const core::StorageKey& key) override;
    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    download(const core::StorageKey& key, const std::filesystem::path& destination) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    upload(const std::filesystem::path& source, const core::StorageKey& key,
           const core::ContentType& type) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    upload_new(const std::filesystem::path& source, const core::StorageKey& key,
               const core::ContentType& type) override;
    // Written to a temporary beside the object and renamed over it at the commit.
    [[nodiscard]] std::expected<std::unique_ptr<core::ports::IObjectStream>,
                                core::ports::StorageError>
    begin(const core::StorageKey& key, const core::ContentType& type,
          std::uint64_t max_bytes) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    remove(const core::StorageKey& key) override;

private:
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    place(const std::filesystem::path& source, const core::StorageKey& key, bool create_only);

    [[nodiscard]] std::filesystem::path object_path(const core::StorageKey& key) const;

    std::filesystem::path objects_;
};

} // namespace infra::storage
