#include "part_session.hpp"

#include "infra/s3util/sigv4.hpp"
#include "infra/s3util/url.hpp"

#include "failure.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace infra::storage::s3 {

using core::ports::IngestState;
using core::ports::StorageError;

PartSession::PartSession(const SessionDeps& deps, core::ports::IngestId id, std::uint64_t offset,
                         core::ports::IIngestObserver& observer)
    : deps_(deps), id_(std::move(id)), observer_(observer), ring_(kBufferBytes), next_(offset),
      part_start_(offset), durable_(offset) {}

PartSession::~PartSession() {
    abort();
}

std::size_t PartSession::write(std::span<const std::byte> bytes) noexcept {
    if (state_ != IngestState::Open) {
        return 0;
    }
    const auto n =
        std::min<std::uint64_t>({bytes.size(), kBufferBytes - buffered_, id_.total_bytes - next_});
    if (n < bytes.size() && next_ + n < id_.total_bytes) {
        blocked_ = true;
    }
    if (n == 0) {
        return 0;
    }
    const std::size_t tail = (head_ + buffered_) % kBufferBytes;
    const std::size_t first = std::min(n, kBufferBytes - tail);
    std::ranges::copy(bytes.first(first), ring_.begin() + static_cast<std::ptrdiff_t>(tail));
    std::ranges::copy(bytes.subspan(first, n - first), ring_.begin());
    buffered_ += n;
    next_ += n;
    // Everything above is settled before libcurl hears of it: resume_body() may call
    // read_body() before it returns.
    if (transfer_ == nullptr) {
        advance();
        if (state_ == IngestState::Failed) {
            notify_later();
        }
    } else if (sent_ < part_length()) {
        transfer_->resume_body();
    }
    return n;
}

bool PartSession::wants_more() const noexcept {
    return state_ == IngestState::Open && next_ < id_.total_bytes && buffered_ < kBufferBytes;
}

void PartSession::finish() noexcept {
    if (state_ != IngestState::Open) {
        return;
    }
    state_ = IngestState::Finalizing;
    if (transfer_ != nullptr && next_ < part_end()) {
        // The part can never reach its declared length. Removing the request makes libcurl
        // close the connection mid-body, and S3 discards a part it did not receive whole.
        transfer_.reset();
        drop_buffered();
    }
    if (transfer_ == nullptr) {
        advance();
        if (state_ != IngestState::Finalizing) {
            notify_later();
        }
    }
}

void PartSession::abort() noexcept {
    if (aborted_) {
        return;
    }
    aborted_ = true;
    if (timer_ != net::TimerId{}) {
        deps_.reactor.cancel_timer(timer_);
        timer_ = {};
    }
    transfer_.reset();
    drop_buffered();
    if (state_ == IngestState::Open || state_ == IngestState::Finalizing) {
        state_ = IngestState::Failed;
    }
}

std::size_t PartSession::read_body(std::span<std::byte> out) noexcept {
    const auto n = std::min<std::uint64_t>({out.size(), buffered_, part_length() - sent_});
    const std::size_t first = std::min(n, kBufferBytes - head_);
    std::ranges::copy(std::span(ring_).subspan(head_, first), out.begin());
    std::ranges::copy(std::span(ring_).first(n - first),
                      out.begin() + static_cast<std::ptrdiff_t>(first));
    head_ = (head_ + n) % kBufferBytes;
    buffered_ -= n;
    sent_ += n;
    if (n > 0 && blocked_) {
        blocked_ = false;
        // Inside libcurl, possibly inside write(): the observer hears of the room later.
        notify_later();
    }
    return n;
}

void PartSession::on_transfer_done(curl::Result result) noexcept {
    transfer_.reset();
    if (!result) {
        fail(failed(result.error()).error);
    } else if (!is_success(*result)) {
        fail(failed(*result).error);
    } else if (!result->header("etag")) {
        // Not something an S3 implementation sends; whatever mangled it may not do so twice.
        fail(StorageError::Transient);
    } else {
        durable_ = part_end();
        part_start_ = durable_;
        sent_ = 0;
        advance();
    }
    // Last: the observer may destroy this session.
    observer_.on_ingest_progress();
}

void PartSession::on_timeout() noexcept {
    timer_ = {};
    observer_.on_ingest_progress();
}

std::uint64_t PartSession::part_length() const noexcept {
    // Parts are cut at chunk_size boundaries of the object, so every part but the last is
    // exactly chunk_size, which is what uniform-part backends (R2) demand. A shorter part
    // only ever goes out when it ends the object.
    return std::min(id_.chunk_size, id_.total_bytes - part_start_);
}

void PartSession::advance() noexcept {
    if (transfer_ != nullptr) {
        return;
    }
    if (state_ == IngestState::Open) {
        if (buffered_ > 0 && !start_part()) {
            fail(StorageError::Permanent);
        }
        return;
    }
    if (state_ != IngestState::Finalizing) {
        return;
    }
    // Once finishing, only a final part that is here in full can still become durable.
    if (buffered_ > 0 && next_ == id_.total_bytes && buffered_ == next_ - part_start_) {
        if (!start_part()) {
            fail(StorageError::Permanent);
        }
        return;
    }
    drop_buffered();
    state_ = IngestState::Committed;
}

bool PartSession::start_part() noexcept {
    const std::uint64_t number = (part_start_ / id_.chunk_size) + 1;
    std::vector<s3util::QueryParam> query{
        {.name = "partNumber", .value = std::to_string(number)},
        {.name = "uploadId", .value = id_.backend_ref},
    };
    // ADR-0005: the body streams in as it arrives, so its hash cannot precede it.
    const curl::Request request = deps_.endpoint.sign(
        curl::Method::Put, deps_.endpoint.bucket().object(id_.key, std::move(query)), {},
        s3util::kUnsignedPayload, 0);
    auto transfer = curl::Transfer::start_upload(deps_.multi, request, part_length(), *this, *this);
    if (!transfer) {
        return false;
    }
    transfer_ = std::move(*transfer);
    sent_ = 0;
    return true;
}

void PartSession::fail(StorageError error) noexcept {
    transfer_.reset();
    drop_buffered();
    error_ = error;
    state_ = IngestState::Failed;
}

void PartSession::drop_buffered() noexcept {
    head_ = 0;
    buffered_ = 0;
    sent_ = 0;
    blocked_ = false;
}

void PartSession::notify_later() noexcept {
    if (timer_ == net::TimerId{} && !aborted_) {
        timer_ = deps_.reactor.arm_timer(core::Millis{0}, *this);
    }
}

} // namespace infra::storage::s3
