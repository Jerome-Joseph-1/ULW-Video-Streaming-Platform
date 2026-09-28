#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace net::detail {

// 16 KiB matches the largest TLS record, so an encrypted record never straddles two chunks.
inline constexpr std::size_t kChunkSize = std::size_t{16} * 1024;

// The largest legitimate response is a rewritten media playlist for a 6 h video: 5400 segments
// x ~400 bytes of presigned URL = 2.2 MB. A queue past 4 MiB belongs to a peer that stopped
// reading, and both reactors fail the connection with ENOBUFS.
inline constexpr std::size_t kMaxSendQueue = std::size_t{4} * 1024 * 1024;

// `data` is left uninitialised: zeroing 16 KiB per chunk buys nothing when bytes are only
// read after being written.
struct Chunk { // NOLINT(cppcoreguidelines-pro-type-member-init)
    Chunk* next = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::array<std::byte, kChunkSize> data;
};

// Per-reactor free list. Chunks are only allocated while the pool grows to its working set;
// after warm-up the data path recycles them.
class ChunkPool {
public:
    ChunkPool() = default;
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    ~ChunkPool() = default;

    [[nodiscard]] Chunk* acquire();
    void release(Chunk* chunk) noexcept;
    [[nodiscard]] std::size_t bytes_in_use() const noexcept { return in_use_ * kChunkSize; }

private:
    std::vector<std::unique_ptr<Chunk>> owned_;
    Chunk* free_ = nullptr;
    std::size_t in_use_ = 0;
};

class ByteQueue {
public:
    ByteQueue() = default;
    ByteQueue(const ByteQueue&) = delete;
    ByteQueue& operator=(const ByteQueue&) = delete;
    ~ByteQueue();

    void append(std::span<const std::byte> bytes, ChunkPool& pool);
    [[nodiscard]] std::span<const std::byte> front() const noexcept;
    // Fills `out` with the queued spans in order, for writev.
    [[nodiscard]] std::size_t gather(std::span<std::span<const std::byte>> out) const noexcept;
    void consume(std::size_t n, ChunkPool& pool) noexcept;
    void clear(ChunkPool& pool) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return bytes_; }
    [[nodiscard]] bool empty() const noexcept { return bytes_ == 0; }

private:
    Chunk* head_ = nullptr;
    Chunk* tail_ = nullptr;
    std::size_t bytes_ = 0;
};

} // namespace net::detail
