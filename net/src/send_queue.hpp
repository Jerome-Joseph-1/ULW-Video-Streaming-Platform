#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

// Released chunks a pool keeps for reuse. A reactor's queues hold a few chunks between them
// in steady traffic; a burst (a slow reader's backlog, a history page, the catch-up after a
// stall) can hold hundreds, and only this many of those are kept once it drains.
inline constexpr std::size_t kKeptChunks = 16;

// Per-reactor free list. Each chunk is a mapping of its own: a chunk released while the pool
// already keeps kKeptChunks is unmapped, so a burst's queues return their memory to the kernel
// when they drain. Chunks kept for good would stay resident at the largest burst the process
// ever queued, a high-water mark that rises, for hours, with each rarer burst; and chunks from
// the heap, once freed, would leave holes that little else fits.
class ChunkPool {
public:
    explicit ChunkPool(std::size_t keep = kKeptChunks) noexcept : keep_(keep) {}
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    ~ChunkPool();

    // Throws std::bad_alloc when no chunk can be mapped.
    [[nodiscard]] Chunk* acquire();
    void release(Chunk* chunk) noexcept;
    [[nodiscard]] std::size_t bytes_in_use() const noexcept { return in_use_ * kChunkSize; }
    [[nodiscard]] std::size_t kept_chunks() const noexcept { return kept_; }

private:
    Chunk* free_ = nullptr;
    std::size_t kept_ = 0;
    std::size_t in_use_ = 0;
    std::size_t keep_;
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
