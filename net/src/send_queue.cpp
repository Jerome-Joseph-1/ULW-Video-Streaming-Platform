#include "send_queue.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

namespace net::detail {

namespace {

void unmap(Chunk* chunk) noexcept {
    ::munmap(chunk->data.data(), kChunkSize);
    delete chunk; // NOLINT(cppcoreguidelines-owning-memory): the pool owns its chunks
}

} // namespace

ChunkPool::~ChunkPool() {
    while (free_ != nullptr) {
        Chunk* c = free_;
        free_ = c->next;
        unmap(c);
    }
}

Chunk* ChunkPool::acquire() {
    Chunk* c = free_;
    if (c != nullptr) {
        free_ = c->next;
        --kept_;
    } else {
        void* mem =
            ::mmap(nullptr, kChunkSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            throw std::bad_alloc();
        }
        try {
            c = new Chunk; // NOLINT(cppcoreguidelines-owning-memory): unmap() deletes it
        } catch (...) {
            ::munmap(mem, kChunkSize);
            throw;
        }
        c->data = {static_cast<std::byte*>(mem), kChunkSize};
        ++mapped_;
    }
    c->next = nullptr;
    c->begin = c->end = 0;
    ++in_use_;
    peak_ = std::max(peak_, in_use_);
    return c;
}

std::size_t ChunkPool::target() const noexcept {
    return std::max({keep_, peak_, last_peak_});
}

void ChunkPool::trim() noexcept {
    while (kept_ > target()) {
        Chunk* c = free_;
        free_ = c->next;
        --kept_;
        unmap(c);
    }
}

void ChunkPool::release(Chunk* chunk) noexcept {
    --in_use_;
    if (++released_ == kPoolWindow) {
        released_ = 0;
        last_peak_ = peak_;
        peak_ = in_use_;
        trim();
    }
    if (kept_ >= target()) {
        unmap(chunk);
        return;
    }
    chunk->next = free_;
    free_ = chunk;
    ++kept_;
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
        std::memcpy(tail_->data.subspan(tail_->end).data(), bytes.data(), n);
        tail_->end += static_cast<std::uint32_t>(n);
        bytes_ += n;
        bytes = bytes.subspan(n);
    }
}

std::span<const std::byte> ByteQueue::front() const noexcept {
    if (head_ == nullptr) {
        return {};
    }
    return head_->data.subspan(head_->begin, head_->end - head_->begin);
}

std::size_t ByteQueue::gather(std::span<std::span<const std::byte>> out) const noexcept {
    std::size_t n = 0;
    for (const Chunk* c = head_; c != nullptr && n < out.size(); c = c->next) {
        out[n++] = c->data.subspan(c->begin, c->end - c->begin);
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
