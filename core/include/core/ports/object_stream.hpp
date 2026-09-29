#pragma once

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "core/ports/storage.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <span>

namespace core::ports {

// One object written front to back whose length is known only at its end, such as a live
// stream's recording remuxed while it is read. Every call blocks.
class IObjectStream {
public:
    virtual ~IObjectStream() = default;
    // Bytes are held until the store takes a piece of the object; memory stays bounded by that
    // piece whatever the object's length.
    [[nodiscard]] virtual std::expected<void, StorageError>
    write(std::span<const std::byte> bytes) = 0;
    // The object appears whole under its key, replacing any there. Nothing of it is visible
    // before, and a stream destroyed without a commit leaves nothing behind.
    [[nodiscard]] virtual std::expected<void, StorageError> commit() = 0;
};

class IObjectStreams {
public:
    virtual ~IObjectStreams() = default;
    [[nodiscard]] virtual std::expected<std::unique_ptr<IObjectStream>, StorageError>
    begin(const StorageKey& key, const ContentType& type) = 0;
};

} // namespace core::ports
