#include "infra/storage/fake_store.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace infra::storage {

using core::ports::IngestId;
using core::ports::IngestState;
using core::ports::StorageError;

class FakeStore::Session final : public core::ports::IIngestSession, public net::ITimerHandler {
public:
    Session(FakeStore& store, IngestId id, std::uint64_t offset,
            core::ports::IIngestObserver& observer)
        : store_(store), id_(std::move(id)), observer_(observer), durable_(offset), next_(offset) {
        store_.sessions_.insert(this);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() override {
        abort();
        store_.sessions_.erase(this);
    }

    // The writer hears from the store one loop turn later, as it would after any progress.
    void wake() noexcept {
        if (state_ == IngestState::Open) {
            schedule();
        }
    }

    std::size_t write(std::span<const std::byte> bytes) noexcept override {
        if (state_ != IngestState::Open) {
            return 0;
        }
        if (!resume_checked_) {
            resume_checked_ = true;
            // Where open() left it, as an object store must: finding out needs a listing.
            if (const auto err = store_.check_resume(id_.backend_ref, next_)) {
                error_ = err;
                state_ = IngestState::Failed;
                schedule();
                return 0;
            }
        }
        FaultPlan plan;
        {
            const std::scoped_lock lock(store_.mutex_);
            plan = store_.plan_;
        }
        if (plan.accept_zero) {
            return 0;
        }
        const auto limit =
            std::min<std::uint64_t>({bytes.size(), plan.accept_per_call, id_.total_bytes - next_});
        std::size_t taken = 0;
        while (taken < limit) {
            const std::uint64_t chunk_len = filling_length();
            if (filling_.size() == chunk_len) {
                break;
            }
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(limit - taken, chunk_len - filling_.size()));
            filling_.insert(filling_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(taken),
                            bytes.begin() + static_cast<std::ptrdiff_t>(taken + n));
            taken += n;
            next_ += n;
            if (filling_.size() == chunk_len) {
                if (uploading_) {
                    // One chunk in flight at a time; the next waits, and so does the writer.
                    break;
                }
                start_upload();
            }
        }
        if (plan.accept_per_call < bytes.size() && taken == plan.accept_per_call) {
            // Capped by the plan rather than by a chunk in flight, so no completion is coming
            // to wake the writer; arrange one.
            schedule();
        }
        return taken;
    }

    [[nodiscard]] bool wants_more() const noexcept override {
        const bool chunk_waiting = uploading_ && filling_.size() == filling_length();
        return state_ == IngestState::Open && next_ < id_.total_bytes && !chunk_waiting;
    }

    void finish() noexcept override {
        if (state_ != IngestState::Open) {
            return;
        }
        state_ = IngestState::Finalizing;
        schedule();
    }

    [[nodiscard]] IngestState state() const noexcept override { return state_; }
    [[nodiscard]] std::uint64_t durable_offset() const noexcept override { return durable_; }
    [[nodiscard]] std::optional<StorageError> error() const noexcept override { return error_; }

    void abort() noexcept override {
        if (aborted_) {
            return;
        }
        aborted_ = true;
        store_.reactor_.cancel_timer(timer_);
        timer_ = {};
        if (state_ == IngestState::Open || state_ == IngestState::Finalizing) {
            state_ = IngestState::Failed;
        }
    }

    // Everything the backend would do asynchronously happens here, one loop turn later, so
    // the observer is never called from inside a session call.
    void on_timeout() noexcept override {
        timer_ = {};
        if (uploading_) {
            StorageError err{};
            const Attempt a =
                store_.store_chunk(id_.backend_ref, uploading_->index, uploading_->bytes, err);
            switch (a) {
            case Attempt::Stored:
                durable_ += uploading_->length;
                uploading_.reset();
                if (!filling_.empty() && filling_.size() == filling_length()) {
                    start_upload();
                }
                break;
            case Attempt::Throttled:
                schedule();
                return;
            case Attempt::Failed:
                uploading_.reset();
                filling_.clear();
                error_ = err;
                state_ = IngestState::Failed;
                observer_.on_ingest_progress();
                return;
            }
        }
        if (state_ == IngestState::Finalizing && !uploading_) {
            // Bytes short of a whole chunk never become durable, exactly as an S3 part upload
            // cut off before its declared Content-Length is abandoned.
            filling_.clear();
            state_ = IngestState::Committed;
        }
        observer_.on_ingest_progress();
    }

private:
    struct Upload {
        std::uint64_t index;
        std::uint64_t length;
        std::vector<std::byte> bytes;
    };

    // The chunk filling_ holds starts where its bytes do, not at next_: once it is full,
    // next_ is already the first byte of the chunk after it, which may be the short tail.
    [[nodiscard]] std::uint64_t filling_index() const noexcept {
        return (next_ - filling_.size()) / id_.chunk_size;
    }

    [[nodiscard]] std::uint64_t filling_length() const noexcept {
        return store_.chunk_length(id_.total_bytes, filling_index());
    }

    void start_upload() noexcept {
        const std::uint64_t length = filling_.size();
        const std::uint64_t index = filling_index();
        uploading_ = Upload{.index = index, .length = length, .bytes = std::move(filling_)};
        filling_.clear();
        schedule();
    }

    void schedule() noexcept {
        if (timer_ == net::TimerId{}) {
            timer_ = store_.reactor_.arm_timer(core::Millis{0}, *this);
        }
    }

    FakeStore& store_;
    IngestId id_;
    core::ports::IIngestObserver& observer_;
    std::uint64_t durable_;
    std::uint64_t next_;
    std::vector<std::byte> filling_;
    std::optional<Upload> uploading_;
    IngestState state_ = IngestState::Open;
    std::optional<StorageError> error_;
    net::TimerId timer_;
    bool resume_checked_ = false;
    bool aborted_ = false;
};

FakeStore::FakeStore(net::IReactor& reactor, const core::ports::IClock& clock,
                     std::uint64_t chunk_size, FaultPlan plan)
    : reactor_(reactor), clock_(clock), chunk_size_(chunk_size), plan_(plan) {}

FakeStore::~FakeStore() = default;

std::uint64_t FakeStore::chunk_length(std::uint64_t total, std::uint64_t index) const noexcept {
    const std::uint64_t start = index * chunk_size_;
    return start >= total ? 0 : std::min(chunk_size_, total - start);
}

std::uint64_t FakeStore::contiguous_bytes(const Ingest& ingest) noexcept {
    std::uint64_t bytes = 0;
    for (std::uint64_t i = 0;; ++i) {
        const auto it = ingest.chunks.find(i);
        if (it == ingest.chunks.end()) {
            return bytes;
        }
        bytes += it->second.size();
    }
}

std::expected<IngestId, StorageError> FakeStore::create(const core::StorageKey& key,
                                                        std::uint64_t total_bytes,
                                                        const core::ContentType& /*type*/) {
    const std::scoped_lock lock(mutex_);
    std::string ref = "fake-" + std::to_string(next_ref_++);
    ingests_.emplace(
        ref, Ingest{.key = key, .total = total_bytes, .created = clock_.wall_now(), .chunks = {}});
    return IngestId{.key = key,
                    .backend_ref = std::move(ref),
                    .total_bytes = total_bytes,
                    .chunk_size = chunk_size_};
}

std::expected<std::unique_ptr<core::ports::IIngestSession>, StorageError>
FakeStore::open(const IngestId& id, std::uint64_t offset, core::ports::IIngestObserver& observer) {
    {
        const std::scoped_lock lock(mutex_);
        const auto it = ingests_.find(id.backend_ref);
        if (it == ingests_.end()) {
            return std::unexpected(StorageError::NotFound);
        }
        // Only offsets the fake could never have reported; whether this one is durable is
        // the session's to find out, as it would be on a real bucket.
        if (id.chunk_size == 0 || offset > it->second.total ||
            (offset % chunk_size_ != 0 && offset != it->second.total)) {
            return std::unexpected(StorageError::PreconditionFailed);
        }
    }
    return std::make_unique<Session>(*this, id, offset, observer);
}

std::optional<StorageError> FakeStore::check_resume(const std::string& ref,
                                                    std::uint64_t offset) const {
    const std::scoped_lock lock(mutex_);
    const auto it = ingests_.find(ref);
    if (it == ingests_.end()) {
        return StorageError::NotFound;
    }
    if (offset > contiguous_bytes(it->second)) {
        return StorageError::PreconditionFailed;
    }
    return std::nullopt;
}

FakeStore::Attempt FakeStore::store_chunk(const std::string& ref, std::uint64_t index,
                                          const std::vector<std::byte>& bytes,
                                          StorageError& error) {
    const std::scoped_lock lock(mutex_);
    ++attempts_;
    if (throttled_ < plan_.throttle_first) {
        ++throttled_;
        return Attempt::Throttled;
    }
    if (plan_.fail_chunk != 0 && index + 1 == plan_.fail_chunk) {
        error = plan_.fail_error;
        return Attempt::Failed;
    }
    const auto it = ingests_.find(ref);
    if (it == ingests_.end()) {
        error = StorageError::NotFound;
        return Attempt::Failed;
    }
    // A re-sent chunk replaces the stored one, as a re-uploaded part does.
    it->second.chunks[index] = bytes;
    return Attempt::Stored;
}

std::expected<std::uint64_t, StorageError> FakeStore::durable_offset(const IngestId& id) {
    const std::scoped_lock lock(mutex_);
    const auto it = ingests_.find(id.backend_ref);
    if (it == ingests_.end()) {
        return committed_.contains(id.backend_ref)
                   ? std::expected<std::uint64_t, StorageError>(id.total_bytes)
                   : std::unexpected(StorageError::NotFound);
    }
    return contiguous_bytes(it->second);
}

std::expected<void, StorageError> FakeStore::commit(const IngestId& id) {
    const std::scoped_lock lock(mutex_);
    const auto it = ingests_.find(id.backend_ref);
    if (it == ingests_.end()) {
        if (committed_.contains(id.backend_ref)) {
            return {};
        }
        return std::unexpected(StorageError::NotFound);
    }
    if (contiguous_bytes(it->second) != it->second.total) {
        return std::unexpected(StorageError::PreconditionFailed);
    }
    std::vector<std::byte> object;
    object.reserve(it->second.total);
    for (const auto& [index, bytes] : it->second.chunks) {
        object.insert(object.end(), bytes.begin(), bytes.end());
    }
    objects_[it->second.key.str()] = std::move(object);
    committed_.insert(id.backend_ref);
    ingests_.erase(it);
    return {};
}

void FakeStore::discard(const IngestId& id) noexcept {
    const std::scoped_lock lock(mutex_);
    ingests_.erase(id.backend_ref);
}

std::expected<core::ports::ReadGrant, StorageError>
FakeStore::grant_read(const core::StorageKey& key, core::Seconds ttl) {
    return core::ports::ReadGrant{.kind = core::ports::ReadGrant::Kind::RedirectUrl,
                                  .value = "fake://" + key.str(),
                                  .ttl = ttl};
}

std::expected<std::vector<std::byte>, StorageError>
FakeStore::fetch_small(const core::StorageKey& key, std::size_t max) {
    {
        std::unique_lock hold(hold_mutex_);
        ++held_;
        hold_released_.wait(hold, [this] { return !hold_; });
        --held_;
    }
    const std::scoped_lock lock(mutex_);
    if (plan_.fail_fetch) {
        return std::unexpected(*plan_.fail_fetch);
    }
    const auto it = objects_.find(key.str());
    if (it == objects_.end()) {
        return std::unexpected(StorageError::NotFound);
    }
    if (it->second.size() > max) {
        return std::unexpected(StorageError::Permanent);
    }
    return it->second;
}

void FakeStore::hold_fetches(bool held) {
    {
        const std::scoped_lock lock(hold_mutex_);
        hold_ = held;
    }
    hold_released_.notify_all();
}

std::size_t FakeStore::held_fetches() const {
    const std::scoped_lock lock(hold_mutex_);
    return hold_ ? held_ : 0;
}

std::expected<void, StorageError> FakeStore::put(const core::StorageKey& key,
                                                 std::span<const std::byte> bytes) {
    const std::scoped_lock lock(mutex_);
    objects_[key.str()].assign(bytes.begin(), bytes.end());
    return {};
}

std::expected<void, StorageError> FakeStore::remove(const core::StorageKey& key) {
    const std::scoped_lock lock(mutex_);
    objects_.erase(key.str());
    return {};
}

std::expected<std::vector<core::StorageKey>, StorageError>
FakeStore::list(std::string_view prefix) {
    const std::scoped_lock lock(mutex_);
    std::vector<core::StorageKey> keys;
    for (const auto& [name, bytes] : objects_) {
        if (name.starts_with(prefix)) {
            if (auto k = core::StorageKey::parse(name)) {
                keys.push_back(*std::move(k));
            }
        }
    }
    return keys;
}

std::expected<std::size_t, StorageError> FakeStore::reap_abandoned(core::WallTime older_than) {
    const std::scoped_lock lock(mutex_);
    return std::erase_if(ingests_, [&](const auto& kv) { return kv.second.created < older_than; });
}

void FakeStore::set_plan(const FaultPlan& plan) {
    const std::scoped_lock lock(mutex_);
    plan_ = plan;
    throttled_ = 0;
}

void FakeStore::wake_writers() noexcept {
    for (Session* const session : sessions_) {
        session->wake();
    }
}

std::size_t FakeStore::chunk_attempts() const {
    const std::scoped_lock lock(mutex_);
    return attempts_;
}

} // namespace infra::storage
