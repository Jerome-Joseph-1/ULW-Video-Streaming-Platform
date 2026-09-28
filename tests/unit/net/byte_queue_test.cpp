#include "send_queue.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace {

using net::detail::ByteQueue;
using net::detail::ChunkPool;
using net::detail::kChunkSize;

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
    for (std::size_t i = 0; i < n; ++i) {
        joined.insert(joined.end(), spans[i].begin(), spans[i].end());
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

} // namespace
