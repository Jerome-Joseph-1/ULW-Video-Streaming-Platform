#include "send_queue.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <gtest/gtest.h>
#include <span>
#include <unistd.h>
#include <vector>

namespace {

using net::detail::ByteQueue;
using net::detail::Chunk;
using net::detail::ChunkPool;
using net::detail::kChunkSize;
using net::detail::kKeptChunks;
using net::detail::kPoolWindow;

std::vector<std::byte> pattern(std::size_t n) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>((i * 31 + 7) % 251);
    }
    return v;
}

std::vector<std::byte> drain(ByteQueue& q, ChunkPool& pool, std::size_t step) {
    std::vector<std::byte> out;
    while (!q.empty()) {
        const auto head = q.front();
        const std::size_t n = std::min(step, head.size());
        out.insert(out.end(), head.begin(), head.begin() + static_cast<std::ptrdiff_t>(n));
        q.consume(n, pool);
    }
    return out;
}

TEST(ByteQueue, PreservesOrderAcrossChunksAndPartialConsumes) {
    ChunkPool pool;
    ByteQueue q;
    const auto data = pattern((3 * kChunkSize) + 123);
    q.append({data.data(), 1000}, pool);
    q.append(std::span(data).subspan(1000), pool);
    EXPECT_EQ(q.size(), data.size());
    EXPECT_EQ(drain(q, pool, 777), data);
    EXPECT_EQ(pool.bytes_in_use(), 0U);
}

TEST(ByteQueue, GatherReturnsEveryChunkInOrder) {
    ChunkPool pool;
    ByteQueue q;
    const auto data = pattern((2 * kChunkSize) + 10);
    q.append(data, pool);
    q.consume(5, pool);
    std::array<std::span<const std::byte>, 8> spans{};
    const std::size_t n = q.gather(spans);
    ASSERT_EQ(n, 3U);
    std::vector<std::byte> joined;
    for (const auto& s : std::span(spans).first(n)) {
        joined.insert(joined.end(), s.begin(), s.end());
    }
    EXPECT_EQ(joined, std::vector<std::byte>(data.begin() + 5, data.end()));
    q.clear(pool);
}

TEST(ByteQueue, ChunksAreRecycledNotReallocated) {
    ChunkPool pool;
    ByteQueue q;
    const auto data = pattern(kChunkSize);
    q.append(data, pool);
    const auto* first = q.front().data();
    q.consume(kChunkSize, pool);
    EXPECT_EQ(pool.bytes_in_use(), 0U);
    q.append(data, pool);
    EXPECT_EQ(q.front().data(), first);
    q.clear(pool);
}

TEST(ChunkPool, ABurstThatRecursFindsItsChunksKept) {
    ChunkPool pool;
    ByteQueue q;
    const auto data = pattern(((3 * kKeptChunks) + 1) * kChunkSize);
    q.append(data, pool);
    EXPECT_EQ(pool.bytes_in_use(), ((3 * kKeptChunks) + 1) * kChunkSize);
    EXPECT_EQ(pool.kept_chunks(), 0U);
    EXPECT_EQ(pool.mapped_chunks(), (3 * kKeptChunks) + 1);
    EXPECT_EQ(drain(q, pool, kChunkSize / 3), data);
    EXPECT_EQ(pool.bytes_in_use(), 0U);
    EXPECT_EQ(pool.kept_chunks(), (3 * kKeptChunks) + 1);
    // The same burst again, as chat's fan-out of one large message is: nothing new is mapped.
    for (int round = 0; round < 5; ++round) {
        q.append(data, pool);
        EXPECT_EQ(drain(q, pool, kChunkSize), data);
    }
    EXPECT_EQ(pool.mapped_chunks(), (3 * kKeptChunks) + 1);
}

// Two windows of releases with one chunk in use at a time.
void quiet(ChunkPool& pool) {
    for (std::size_t i = 0; i < 2 * kPoolWindow; ++i) {
        pool.release(pool.acquire());
    }
}

TEST(ChunkPool, ABurstThatDoesNotRecurIsGivenBackTwoWindowsLater) {
    ChunkPool pool;
    ByteQueue q;
    const auto data = pattern(200 * kChunkSize);
    q.append(data, pool);
    EXPECT_EQ(drain(q, pool, kChunkSize), data);
    EXPECT_EQ(pool.kept_chunks(), 200U);
    quiet(pool);
    EXPECT_EQ(pool.kept_chunks(), kKeptChunks);
    // The quiet traffic was served from what was kept.
    EXPECT_EQ(pool.mapped_chunks(), 200U);
}

// Whether a mapping still covers the bytes: mincore fails with ENOMEM where none does.
bool mapped(std::byte* b) {
    unsigned char resident = 0;
    return ::mincore(b, 1, &resident) == 0;
}

TEST(ChunkPool, UnmapsTheChunksPastTheKeptOnes) {
    std::vector<Chunk*> chunks;
    std::vector<std::byte*> bytes;
    {
        ChunkPool pool;
        for (std::size_t i = 0; i < 3 * kKeptChunks; ++i) {
            chunks.push_back(pool.acquire());
            std::ranges::fill(chunks.back()->data, std::byte{0x5a});
            bytes.push_back(chunks.back()->data.data());
        }
        for (std::byte* b : bytes) {
            EXPECT_TRUE(mapped(b));
        }
        for (Chunk* c : chunks) {
            pool.release(c);
        }
        quiet(pool);
        EXPECT_EQ(pool.kept_chunks(), kKeptChunks);
        EXPECT_EQ(pool.mapped_chunks(), 3 * kKeptChunks);
        EXPECT_EQ(static_cast<std::size_t>(std::ranges::count_if(bytes, mapped)), kKeptChunks);
    }
    // And the kept ones go with the pool.
    for (std::byte* b : bytes) {
        EXPECT_FALSE(mapped(b));
    }
}

TEST(ChunkPool, ServesEveryBurstWhateverTheOnesBeforeLeft) {
    ChunkPool pool;
    ByteQueue q;
    std::size_t largest = 0;
    for (const std::size_t chunks : {1U, 40U, 3U, 200U, 0U, 17U}) {
        const auto data = pattern((chunks * kChunkSize) + 5);
        largest = std::max(largest, chunks + 1);
        q.append(data, pool);
        EXPECT_EQ(pool.bytes_in_use(), (chunks + 1) * kChunkSize);
        EXPECT_EQ(drain(q, pool, 3000), data);
        EXPECT_EQ(pool.bytes_in_use(), 0U);
        EXPECT_LE(pool.kept_chunks(), std::max(kKeptChunks, largest));
    }
}

TEST(ChunkPool, KeepsOnlyTheRecentPeakWhenAskedToKeepNone) {
    ChunkPool pool(0);
    ByteQueue q;
    const auto data = pattern(kChunkSize + 1);
    q.append(data, pool);
    EXPECT_EQ(drain(q, pool, 100), data);
    EXPECT_EQ(pool.bytes_in_use(), 0U);
    EXPECT_EQ(pool.kept_chunks(), 2U);
    for (std::size_t i = 0; i < 2 * kPoolWindow; ++i) {
        q.append(std::span(data).first(1), pool);
        q.clear(pool);
    }
    EXPECT_EQ(pool.kept_chunks(), 1U);
}

} // namespace
