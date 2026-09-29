#pragma once

#include "core/ports/object_transfer.hpp"

#include <filesystem>

namespace infra::storage {

// IObjectTransfer over FsStore's layout (objects/<key> under the same root), so a worker in
// development reads what a gateway on the same root committed. Needs no reactor.
class FsTransfer final : public core::ports::IObjectTransfer {
public:
    explicit FsTransfer(const std::filesystem::path& root);

    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    size(const core::StorageKey& key) override;
    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    download(const core::StorageKey& key, const std::filesystem::path& destination) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    upload(const std::filesystem::path& source, const core::StorageKey& key,
           const core::ContentType& type) override;

private:
    [[nodiscard]] std::filesystem::path object_path(const core::StorageKey& key) const;

    std::filesystem::path objects_;
};

} // namespace infra::storage
