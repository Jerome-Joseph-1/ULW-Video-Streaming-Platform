#pragma once

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "core/ports/storage.hpp"

#include <cstddef>
#include <cstdint>
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
    // The object appears whole under its key. Nothing of it is visible before, and a stream
    // destroyed without a commit leaves nothing behind.
    [[nodiscard]] virtual std::expected<void, StorageError> commit() = 0;
};

class IObjectStreams {
public:
    virtual ~IObjectStreams() = default;
    // `max_bytes` bounds what the stream will be written, and sizes the pieces it goes up in;
    // the stream fails past it.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IObjectStream>, StorageError>
    begin(const StorageKey& key, const ContentType& type, std::uint64_t max_bytes) = 0;
    // Removes an object a committed stream left that nothing is to read. Idempotent: an
    // object already gone counts as removed.
    [[nodiscard]] virtual std::expected<void, StorageError> remove(const StorageKey& key) = 0;
};

} // namespace core::ports
