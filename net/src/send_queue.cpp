#include "send_queue.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace net::detail {

Chunk* ChunkPool::acquire() {
    Chunk* c = free_;
    if (c != nullptr) {
        free_ = c->next;
    } else {
        owned_.push_back(std::make_unique<Chunk>());
        c = owned_.back().get();
    }
    c->next = nullptr;
    c->begin = c->end = 0;
    ++in_use_;
    return c;
}

void ChunkPool::release(Chunk* chunk) noexcept {
    chunk->next = free_;
    free_ = chunk;
    --in_use_;
}

ByteQueue::~ByteQueue() {
    // Queues must be cleared back into their pool; destroying a non-empty one leaks chunks
    // from the pool's accounting.
    assert(head_ == nullptr);
}

void ByteQueue::append(std::span<const std::byte> bytes, ChunkPool& pool) {
    while (!bytes.empty()) {
        if (tail_ == nullptr || tail_->end == kChunkSize) {
            Chunk* c = pool.acquire();
            if (tail_ != nullptr) {
                tail_->next = c;
            } else {
                head_ = c;
            }
            tail_ = c;
        }
        const std::size_t n = std::min(bytes.size(), kChunkSize - tail_->end);
        std::memcpy(tail_->data.data() + tail_->end, bytes.data(), n);
        tail_->end += static_cast<std::uint32_t>(n);
        bytes_ += n;
        bytes = bytes.subspan(n);
    }
}

std::span<const std::byte> ByteQueue::front() const noexcept {
    if (head_ == nullptr) {
        return {};
    }
    return {head_->data.data() + head_->begin, head_->end - head_->begin};
}

std::size_t ByteQueue::gather(std::span<std::span<const std::byte>> out) const noexcept {
    std::size_t n = 0;
    for (const Chunk* c = head_; c != nullptr && n < out.size(); c = c->next) {
        out[n++] = {c->data.data() + c->begin, c->end - c->begin};
    }
    return n;
}

void ByteQueue::consume(std::size_t n, ChunkPool& pool) noexcept {
    assert(n <= bytes_);
    bytes_ -= n;
    while (n > 0) {
        const std::size_t avail = head_->end - head_->begin;
        if (n < avail) {
            head_->begin += static_cast<std::uint32_t>(n);
            return;
        }
        n -= avail;
        Chunk* done = head_;
        head_ = head_->next;
        pool.release(done);
    }
    if (head_ == nullptr) {
        tail_ = nullptr;
    }
}

void ByteQueue::clear(ChunkPool& pool) noexcept {
    while (head_ != nullptr) {
        Chunk* done = head_;
        head_ = head_->next;
        pool.release(done);
    }
    tail_ = nullptr;
    bytes_ = 0;
}

} // namespace net::detail
