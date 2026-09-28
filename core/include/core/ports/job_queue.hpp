#pragma once

#include "core/models/ids.hpp"
#include "core/models/storage_key.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace core::ports {

enum class JobQueueError : std::uint8_t {
    // The database is unreachable or refused the statement; the call may be retried.
    Unavailable,
    // A stored row violates a domain invariant.
    Corrupt,
};

template <class T> using JobQueueResult = std::expected<T, JobQueueError>;

enum class JobKind : std::uint8_t { Transcode };

// Internal to the queue and the worker; never shown to clients.
enum class JobId : std::int64_t {};

// A claim on one job. Every claim raises the job's fence, so a worker whose lease lapsed and
// whose job was claimed again holds a stale fence, and every write it attempts is refused.
struct JobLease {
    JobId job{};
    std::uint64_t fence = 0;
};

struct ClaimedJob {
    JobLease lease;
    VideoId video;
    JobKind kind = JobKind::Transcode;
    StorageKey source;
    // Of the request that queued the job, for tracing it from the gateway into the worker.
    std::string request_id;
};

struct Rendition {
    std::uint32_t height = 0;
    std::uint32_t bitrate_bps = 0;
    StorageKey playlist;
};

// A lease lasts kJobLease unless renewed. Beating every kJobHeartbeat leaves room for two
// missed beats before another worker may take the job.
inline constexpr Seconds kJobLease{60};
inline constexpr Seconds kJobHeartbeat{20};

// Work for transcode workers. Every call blocks.
class IJobQueue {
public:
    virtual ~IJobQueue() = default;

    // Leases the oldest job that is due; nullopt when none is.
    [[nodiscard]] virtual JobQueueResult<std::optional<ClaimedJob>> claim(const NodeId& worker) = 0;
    // false: the lease is gone. Abandon the job without writing anything.
    [[nodiscard]] virtual JobQueueResult<bool> heartbeat(const JobLease& lease,
                                                         const NodeId& worker) = 0;
    // `percent` is 0..100.
    [[nodiscard]] virtual JobQueueResult<bool> report_progress(const JobLease& lease,
                                                               std::uint8_t percent) = 0;
    // One transaction: the job is done, its video is ready with `duration`, and the renditions
    // are recorded. false: fenced out, and nothing was written.
    [[nodiscard]] virtual JobQueueResult<bool> finish(const JobLease& lease, Millis duration,
                                                      std::span<const Rendition> renditions) = 0;
    // A retryable failure queues the job again after a backoff while attempts remain;
    // otherwise the job and its video fail with `reason`. false: fenced out.
    [[nodiscard]] virtual JobQueueResult<bool> fail(const JobLease& lease, std::string_view reason,
                                                    bool retryable) = 0;
    // Jobs whose lease lapsed go back to the queue after a backoff, or, out of attempts, fail
    // together with their videos. Returns how many it moved.
    [[nodiscard]] virtual JobQueueResult<std::size_t> reap_expired() = 0;
    // Returns once a job may have been queued, or after `max_wait`. The wakeup is only a hint
    // and can be lost, so callers claim after every return: `max_wait` is the polling interval.
    virtual void wait_for_work(Millis max_wait) = 0;
};

} // namespace core::ports
