#include "infra/storage/fs_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace infra::storage {

namespace fs = std::filesystem;
using core::ports::IngestId;
using core::ports::IngestState;
using core::ports::StorageError;

namespace {

// Two 1 MiB buffers per session: one filling while the other is written out, enough to keep
// a disk busy between wakeups without holding much more than a few receive buffers per upload.
constexpr std::size_t kBufferBytes = std::size_t{1} << 20;

StorageError from_errno(int err) noexcept {
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return StorageError::NotFound;
    case EEXIST:
        return StorageError::AlreadyExists;
    case EACCES:
    case EPERM:
    case EROFS:
        return StorageError::Unauthorized;
    case EINTR:
    case EAGAIN:
    case ENOSPC:
    case EDQUOT:
        return StorageError::Transient;
    default:
        return StorageError::Permanent;
    }
}

StorageError from_error_code(const std::error_code& ec) noexcept {
    return from_errno(ec.value());
}

std::expected<std::string, StorageError> read_text(const fs::path& p, std::size_t max) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::unexpected(StorageError::NotFound);
    }
    std::string text(max, '\0');
    in.read(text.data(), static_cast<std::streamsize>(max));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

std::expected<std::uint64_t, StorageError> read_offset(const fs::path& p) {
    auto text = read_text(p, 32);
    if (!text) {
        return std::unexpected(text.error());
    }
    std::uint64_t value = 0;
    const auto* end = text->data() + text->size();
    const auto [ptr, ec] = std::from_chars(text->data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected(StorageError::Corrupt);
    }
    return value;
}

// Write to a temporary, flush it, rename over the target: readers see the old content or the
// new, never a torn file.
std::expected<void, StorageError> write_atomically(const fs::path& p,
                                                   std::span<const std::byte> bytes) {
    const fs::path tmp = fs::path(p).concat(".tmp");
    const os::UniqueFd fd{::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
    if (!fd) {
        return std::unexpected(from_errno(errno));
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd.get(), bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(from_errno(errno));
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fdatasync(fd.get()) != 0 || ::rename(tmp.c_str(), p.c_str()) != 0) {
        return std::unexpected(from_errno(errno));
    }
    return {};
}

// A marker that cannot be read is an error, not an absent marker.
std::expected<bool, StorageError> committed(const fs::path& dir) {
    std::error_code ec;
    const bool present = fs::exists(dir / "committed", ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return present;
}

std::expected<void, StorageError> write_offset(const fs::path& p, std::uint64_t value) {
    std::array<char, 24> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return write_atomically(p, std::as_bytes(std::span(buf.data(), end)));
}

} // namespace

// One write of buffered bytes, run on the offload pool. It owns the descriptor while it
// runs and hands it back on completion; if its session was aborted meanwhile, the job closes
// the descriptor itself.
class FsStore::WriteJob final : public net::IOffloadJob {
public:
    struct Request {
        fs::path dir;
        os::UniqueFd fd;
        // Where to truncate the file before the first write; only used while fd is unset.
        std::uint64_t open_at = 0;
        std::vector<std::byte> bytes;
        std::uint64_t at = 0;
        std::uint64_t synced = 0;
        std::uint64_t chunk = 0;
        bool final = false;
    };

    WriteJob(FsStore& store, Session& owner, Request request)
        : store_(store), owner_(&owner), req_(std::move(request)) {}

    void run() noexcept override {
        if (!req_.fd) {
            req_.fd = os::UniqueFd{
                ::open((req_.dir / "data").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600)};
            if (!req_.fd) {
                error_ = from_errno(errno);
                return;
            }
            const auto on_disk = read_offset(req_.dir / "durable");
            if (!on_disk) {
                error_ = on_disk.error();
                return;
            }
            if (req_.open_at > *on_disk) {
                error_ = StorageError::PreconditionFailed;
                return;
            }
            // Bytes past the durable offset may be torn by a crash; the resume point wins.
            if (::ftruncate(req_.fd.get(), static_cast<off_t>(req_.open_at)) != 0) {
                error_ = from_errno(errno);
                return;
            }
        }
        std::size_t done = 0;
        while (done < req_.bytes.size()) {
            const ssize_t n =
                ::pwrite(req_.fd.get(), req_.bytes.data() + done, req_.bytes.size() - done,
                         static_cast<off_t>(req_.at + done));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error_ = from_errno(errno);
                return;
            }
            done += static_cast<std::size_t>(n);
        }
        const std::uint64_t end = req_.at + req_.bytes.size();
        // Syncing once per chunk mirrors the object stores, where bytes become durable a
        // part at a time, and keeps fdatasync off the per-buffer path.
        if (req_.final || end / req_.chunk > req_.synced / req_.chunk) {
            if (::fdatasync(req_.fd.get()) != 0) {
                error_ = from_errno(errno);
                return;
            }
            if (auto w = write_offset(req_.dir / "durable", end); !w) {
                error_ = w.error();
                return;
            }
            durable_ = end;
        }
    }

    void complete() noexcept override { store_.finish_job(*this); }

    void detach() noexcept { owner_ = nullptr; }
    [[nodiscard]] Session* owner() const noexcept { return owner_; }
    [[nodiscard]] std::optional<StorageError> error() const noexcept { return error_; }
    [[nodiscard]] std::optional<std::uint64_t> durable() const noexcept { return durable_; }
    [[nodiscard]] bool final() const noexcept { return req_.final; }
    [[nodiscard]] os::UniqueFd take_fd() noexcept { return std::move(req_.fd); }
    [[nodiscard]] std::vector<std::byte> take_buffer() noexcept {
        req_.bytes.clear();
        return std::move(req_.bytes);
    }

private:
    FsStore& store_;
    Session* owner_;
    Request req_;
    std::optional<StorageError> error_;
    std::optional<std::uint64_t> durable_;
};

class FsStore::Session final : public core::ports::IIngestSession {
public:
    Session(FsStore& store, IngestId id, std::uint64_t offset,
            core::ports::IIngestObserver& observer)
        : store_(store), id_(std::move(id)), observer_(observer), open_at_(offset),
          durable_(offset), next_(offset), handed_(offset) {
        front_.reserve(kBufferBytes);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() override { abort(); }

    std::size_t write(std::span<const std::byte> bytes) noexcept override {
        if (state_ != IngestState::Open) {
            return 0;
        }
        const auto n = std::min<std::uint64_t>(
            {bytes.size(), kBufferBytes - front_.size(), id_.total_bytes - next_});
        front_.insert(front_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        next_ += n;
        if (job_ == nullptr && !front_.empty()) {
            submit(false);
        }
        return n;
    }

    [[nodiscard]] bool wants_more() const noexcept override {
        return state_ == IngestState::Open && next_ < id_.total_bytes &&
               front_.size() < kBufferBytes;
    }

    void finish() noexcept override {
        if (state_ != IngestState::Open) {
            return;
        }
        state_ = IngestState::Finalizing;
        if (job_ == nullptr) {
            submit(true);
        }
    }

    [[nodiscard]] IngestState state() const noexcept override { return state_; }
    [[nodiscard]] std::uint64_t durable_offset() const noexcept override { return durable_; }
    [[nodiscard]] std::optional<StorageError> error() const noexcept override { return error_; }

    void abort() noexcept override {
        if (job_ != nullptr) {
            job_->detach();
            job_ = nullptr;
        }
        if (state_ == IngestState::Open || state_ == IngestState::Finalizing) {
            state_ = IngestState::Failed;
        }
    }

    void on_job_done(WriteJob& job) noexcept {
        job_ = nullptr;
        fd_ = job.take_fd();
        spare_ = job.take_buffer();
        if (const auto err = job.error()) {
            error_ = err;
            state_ = IngestState::Failed;
            observer_.on_ingest_progress();
            return;
        }
        if (const auto d = job.durable()) {
            durable_ = *d;
            synced_ = *d;
        }
        if (state_ == IngestState::Finalizing) {
            if (job.final()) {
                state_ = IngestState::Committed;
            } else {
                // No write can arrive once finishing, so this job carries the rest.
                submit(true);
            }
        } else if (!front_.empty()) {
            submit(false);
        }
        observer_.on_ingest_progress();
    }

private:
    void submit(bool finalizing) noexcept {
        std::vector<std::byte> bytes = std::move(front_);
        front_ = std::move(spare_);
        front_.clear();
        if (front_.capacity() < kBufferBytes) {
            front_.reserve(kBufferBytes);
        }
        const std::uint64_t at = handed_;
        handed_ += bytes.size();
        job_ = &store_.adopt_job(
            std::make_unique<WriteJob>(store_, *this,
                                       WriteJob::Request{.dir = store_.ingest_dir(id_.backend_ref),
                                                         .fd = std::move(fd_),
                                                         .open_at = open_at_,
                                                         .bytes = std::move(bytes),
                                                         .at = at,
                                                         .synced = synced_,
                                                         .chunk = id_.chunk_size,
                                                         .final = finalizing}));
    }

    FsStore& store_;
    IngestId id_;
    core::ports::IIngestObserver& observer_;
    std::uint64_t open_at_;
    std::uint64_t durable_;
    std::uint64_t synced_ = durable_;
    std::uint64_t next_;
    std::uint64_t handed_;
    std::vector<std::byte> front_;
    std::vector<std::byte> spare_;
    os::UniqueFd fd_;
    WriteJob* job_ = nullptr;
    IngestState state_ = IngestState::Open;
    std::optional<StorageError> error_;
};

FsStore::FsStore(Deps deps, fs::path root, std::uint64_t chunk_size)
    : deps_(deps), root_(std::move(root)), chunk_size_(chunk_size) {
    if (chunk_size_ == 0) {
        throw std::invalid_argument("fs store chunk size is zero");
    }
}

// Jobs still owned here may be running; the offload pool must be stopped first.
FsStore::~FsStore() = default;

fs::path FsStore::ingest_dir(const std::string& ref) const {
    return root_ / "ingest" / ref;
}

fs::path FsStore::object_path(const core::StorageKey& key) const {
    return root_ / "objects" / key.str();
}

bool FsStore::valid_ref(const std::string& ref) {
    // Refs are ours (hex), but they come back from the database; never let one name a path.
    return ref.size() == 32 && std::ranges::all_of(ref, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

FsStore::WriteJob& FsStore::adopt_job(std::unique_ptr<WriteJob> job_ptr) {
    jobs_.push_back(std::move(job_ptr));
    WriteJob& job = *jobs_.back();
    deps_.pool.submit(job);
    return job;
}

void FsStore::finish_job(WriteJob& job) noexcept {
    if (Session* owner = job.owner()) {
        owner->on_job_done(job);
    }
    std::erase_if(jobs_, [&](const auto& j) { return j.get() == &job; });
}

std::expected<IngestId, StorageError> FsStore::create(const core::StorageKey& key,
                                                      std::uint64_t total_bytes,
                                                      const core::ContentType& /*type*/) {
    std::array<std::byte, 16> raw{};
    deps_.random.fill(raw);
    std::string ref;
    ref.reserve(32);
    for (const std::byte b : raw) {
        constexpr std::string_view kHex = "0123456789abcdef";
        ref.push_back(kHex[std::to_integer<std::size_t>(b) >> 4U]);
        ref.push_back(kHex[std::to_integer<std::size_t>(b) & 0xFU]);
    }
    const fs::path dir = ingest_dir(ref);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    const std::string meta = key.str() + "\n" + std::to_string(total_bytes) + "\n";
    if (auto w = write_atomically(dir / "meta", std::as_bytes(std::span(meta))); !w) {
        return std::unexpected(w.error());
    }
    if (auto w = write_offset(dir / "durable", 0); !w) {
        return std::unexpected(w.error());
    }
    return IngestId{.key = key,
                    .backend_ref = std::move(ref),
                    .total_bytes = total_bytes,
                    .chunk_size = chunk_size_};
}

std::expected<std::unique_ptr<core::ports::IIngestSession>, StorageError>
FsStore::open(const IngestId& id, std::uint64_t offset, core::ports::IIngestObserver& observer) {
    if (!valid_ref(id.backend_ref) || id.chunk_size == 0 || offset > id.total_bytes) {
        return std::unexpected(StorageError::PreconditionFailed);
    }
    // Whether `offset` is really durable is checked by the first write job: finding out here
    // would mean file I/O on the reactor thread.
    return std::make_unique<Session>(*this, id, offset, observer);
}

std::expected<std::uint64_t, StorageError> FsStore::durable_offset(const IngestId& id) {
    if (!valid_ref(id.backend_ref)) {
        return std::unexpected(StorageError::NotFound);
    }
    const fs::path dir = ingest_dir(id.backend_ref);
    const auto done = committed(dir);
    if (!done) {
        return std::unexpected(done.error());
    }
    if (*done) {
        return id.total_bytes;
    }
    return read_offset(dir / "durable");
}

std::expected<void, StorageError> FsStore::commit(const IngestId& id) {
    if (!valid_ref(id.backend_ref)) {
        return std::unexpected(StorageError::NotFound);
    }
    const fs::path dir = ingest_dir(id.backend_ref);
    const auto done = committed(dir);
    if (!done) {
        return std::unexpected(done.error());
    }
    if (*done) {
        return {};
    }
    const auto durable = read_offset(dir / "durable");
    if (!durable) {
        return std::unexpected(durable.error());
    }
    if (*durable != id.total_bytes) {
        return std::unexpected(StorageError::PreconditionFailed);
    }
    const fs::path target = object_path(id.key);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    // A data file longer than the durable offset holds bytes from an abandoned attempt.
    fs::resize_file(dir / "data", id.total_bytes, ec);
    if (!ec) {
        fs::rename(dir / "data", target, ec);
    }
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    // The marker makes a retried commit succeed once the data file has moved.
    return write_atomically(dir / "committed", {});
}

void FsStore::discard(const IngestId& id) noexcept {
    if (!valid_ref(id.backend_ref)) {
        return;
    }
    const fs::path dir = ingest_dir(id.backend_ref);
    // Only when known to be uncommitted: the marker is what lets a retried commit succeed.
    if (committed(dir) == false) {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

std::expected<core::ports::ReadGrant, StorageError> FsStore::grant_read(const core::StorageKey& key,
                                                                        core::Seconds ttl) {
    return core::ports::ReadGrant{.kind = core::ports::ReadGrant::Kind::ServeLocally,
                                  .value = object_path(key).string(),
                                  .ttl = ttl};
}

std::expected<std::vector<std::byte>, StorageError>
FsStore::fetch_small(const core::StorageKey& key, std::size_t max) {
    const fs::path p = object_path(key);
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    if (size > max) {
        return std::unexpected(StorageError::Permanent);
    }
    std::ifstream in(p, std::ios::binary);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) {
        return std::unexpected(StorageError::Transient);
    }
    return bytes;
}

std::expected<void, StorageError> FsStore::put(const core::StorageKey& key,
                                               std::span<const std::byte> bytes) {
    const fs::path p = object_path(key);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return write_atomically(p, bytes);
}

std::expected<void, StorageError> FsStore::remove(const core::StorageKey& key) {
    std::error_code ec;
    fs::remove(object_path(key), ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return {};
}

std::expected<std::vector<core::StorageKey>, StorageError> FsStore::list(std::string_view prefix) {
    std::vector<core::StorageKey> keys;
    const fs::path objects = root_ / "objects";
    std::error_code ec;
    if (!fs::exists(objects, ec)) {
        if (ec) {
            return std::unexpected(from_error_code(ec));
        }
        return keys;
    }
    for (auto it = fs::recursive_directory_iterator(objects, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        const std::string rel = fs::relative(it->path(), objects, ec).generic_string();
        if (rel.starts_with(prefix) && !rel.ends_with(".tmp")) {
            if (auto k = core::StorageKey::parse(rel)) {
                keys.push_back(*std::move(k));
            }
        }
    }
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return keys;
}

std::expected<std::size_t, StorageError> FsStore::reap_abandoned(core::WallTime older_than) {
    const fs::path ingest = root_ / "ingest";
    std::error_code ec;
    if (!fs::exists(ingest, ec)) {
        if (ec) {
            return std::unexpected(from_error_code(ec));
        }
        return 0;
    }
    // file_clock and system_clock share an epoch on libstdc++; clock_cast makes it explicit.
    const auto cutoff = std::chrono::clock_cast<fs::file_time_type::clock>(older_than);
    std::size_t reaped = 0;
    // Incremented by hand: the range-for increment throws on a failed readdir.
    for (auto it = fs::directory_iterator(ingest, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        std::error_code entry_ec;
        const auto meta_time = fs::last_write_time(it->path() / "meta", entry_ec);
        if (entry_ec || meta_time >= cutoff || committed(it->path()) != false) {
            continue;
        }
        fs::remove_all(it->path(), entry_ec);
        if (!entry_ec) {
            ++reaped;
        }
    }
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return reaped;
}

} // namespace infra::storage
