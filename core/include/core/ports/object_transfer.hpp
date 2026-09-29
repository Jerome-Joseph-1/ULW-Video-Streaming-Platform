#pragma once

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "core/ports/storage.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>

namespace core::ports {

// Whole objects between the store and local files, for the transcode worker, which has no
// reactor. Every call blocks. Bytes stream through a bounded buffer, never the whole object:
// a raw upload can be many gigabytes.
class IObjectTransfer {
public:
    virtual ~IObjectTransfer() = default;
    [[nodiscard]] virtual std::expected<std::uint64_t, StorageError>
    size(const StorageKey& key) = 0;
    // Creates or truncates `destination` and returns the bytes written. After a failure the
    // file's contents are unspecified.
    [[nodiscard]] virtual std::expected<std::uint64_t, StorageError>
    download(const StorageKey& key, const std::filesystem::path& destination) = 0;
    // Creates or replaces the object. Readers see the old object or the new one, never a mix.
    [[nodiscard]] virtual std::expected<void, StorageError>
    upload(const std::filesystem::path& source, const StorageKey& key, const ContentType& type) = 0;
    // upload(), but only if the key holds no object: AlreadyExists when it does, and the
    // existing object is left as it was. The store decides, so of several writers racing for a
    // key exactly one wins. For claims that fence one writer off from another, not for data.
    [[nodiscard]] virtual std::expected<void, StorageError>
    upload_new(const std::filesystem::path& source, const StorageKey& key,
               const ContentType& type) = 0;
};

} // namespace core::ports
