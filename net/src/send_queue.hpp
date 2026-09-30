#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace net::detail {

// 16 KiB matches the largest TLS record, so an encrypted record never straddles two chunks.
inline constexpr std::size_t kChunkSize = std::size_t{16} * 1024;
static_assert(kChunkSize % 4096 == 0, "a chunk's bytes are whole pages");

// The largest legitimate response is a rewritten media playlist for a 6 h video: 5400 segments
// x ~400 bytes of presigned URL = 2.2 MB. A queue past 4 MiB belongs to a peer that stopped
// reading, and both reactors fail the connection with ENOBUFS.
inline constexpr std::size_t kMaxSendQueue = std::size_t{4} * 1024 * 1024;

// A chunk's bytes are a mapping of exactly kChunkSize, four pages; the header lives apart, on
// the heap, so it does not add a fifth. `data` is left uninitialised: zeroing 16 KiB per chunk
// buys nothing when bytes are only read after being written.
struct Chunk {
    Chunk* next = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::span<std::byte> data; // kChunkSize bytes
};

// Released chunks a pool keeps at least, however quiet it has been.
inline constexpr std::size_t kKeptChunks = 16;

// Releases per window. Over each, a pool notes the most chunks in use at once; it keeps as
// many released chunks as that peak, over this window and the last, so a burst that recurs
// (chat's fan-out of one large message to every member, a gateway's long playlist) finds its
// chunks kept rather than mapping them again, and a burst that does not is given back two
// windows later. 4096 releases are 64 MiB sent.
inline constexpr std::size_t kPoolWindow = 4096;

// Per-reactor free list. Each chunk's bytes are a mapping of their own, unmapped when a
// released chunk is past what the pool keeps, so a burst's queues return their memory to the
// kernel once the burst is over. Chunks kept for good would stay resident at the largest burst
// the process ever queued, a high-water mark that rises, for hours, with each rarer burst; and
// chunks from the heap, once freed, would leave holes that little else fits. What is kept is
// trimmed when a pool next releases, so a reactor that goes quiet right after a burst holds it
// until traffic resumes.
class ChunkPool {
public:
    explicit ChunkPool(std::size_t keep = kKeptChunks) noexcept : keep_(keep) {}
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    ~ChunkPool();

    // Throws std::bad_alloc when no chunk can be had.
    [[nodiscard]] Chunk* acquire();
    void release(Chunk* chunk) noexcept;
    [[nodiscard]] std::size_t bytes_in_use() const noexcept { return in_use_ * kChunkSize; }
    [[nodiscard]] std::size_t kept_chunks() const noexcept { return kept_; }
    // Chunks mapped over the pool's life, for tests of reuse.
    [[nodiscard]] std::size_t mapped_chunks() const noexcept { return mapped_; }

private:
    [[nodiscard]] std::size_t target() const noexcept;
    void trim() noexcept;

    Chunk* free_ = nullptr;
    std::size_t kept_ = 0;
    std::size_t in_use_ = 0;
    std::size_t keep_;
    std::size_t peak_ = 0;
    std::size_t last_peak_ = 0;
    std::size_t released_ = 0;
    std::size_t mapped_ = 0;
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
