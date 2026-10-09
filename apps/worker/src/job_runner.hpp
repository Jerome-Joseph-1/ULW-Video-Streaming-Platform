#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/job_queue.hpp"
#include "core/ports/object_transfer.hpp"
#include "core/ports/random.hpp"
#include "core/ports/transcoder.hpp"

#include "lease_keeper.hpp"
#include "ops/log.hpp"
#include "workspace.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>

namespace worker {

// What to do about a failed probe, transcode or verification.
enum class Disposition : std::uint8_t {
    // Run the same step again here, once.
    RerunOnce,
    // Give the job back to the queue; another attempt, here or elsewhere, may succeed.
    Requeue,
    // Fail the job and its video.
    FailPermanently,
    // Stop without writing anything more.
    Abandon,
};

// `reran`: this failure is already the rerun's.
[[nodiscard]] Disposition disposition(core::ports::TranscodeFailure failure, bool reran) noexcept;

enum class JobOutcome : std::uint8_t {
    Done,
    Failed,
    Requeued,
    // The lease was lost mid-job, and nothing more was written.
    Abandoned,
    // A fenced write matched no row: another worker holds the job now.
    FencedOut,
    // The final write did not happen: the database was unreachable, or refused the call. The
    // lease will lapse and the reaper requeue or fail the job.
    Unrecorded,
};

[[nodiscard]] std::string_view to_string(JobOutcome outcome) noexcept;
[[nodiscard]] std::string_view to_string(core::ports::JobQueueError error) noexcept;

struct JobDeps {
    core::ports::IJobQueue& queue;
    // A session of its own for the lease keeper's thread.
    core::ports::IJobQueue& lease_queue;
    core::ports::IObjectTransfer& store;
    core::ports::ITranscoder& transcoder;
    const core::ports::IClock& clock;
    core::ports::IRandom& random;
    FreeSpace free_space;
    // Used from the job's thread and the lease keeper's.
    ops::Logger& log;
    // Touched by the lease keeper while a job runs; none in tests that do not look at it.
    const Heartbeat* heartbeat = nullptr;
};

struct JobSettings {
    std::filesystem::path scratch;
    core::NodeId node;
    LeaseKeeper::Intervals lease;
    // Segments uploaded at once while publishing (ULW_PUBLISH_CONCURRENCY, ADR-0102); 1 is the
    // sequential upload, in order.
    unsigned publish_concurrency = 1;
};

// One claimed job, start to finish: workspace, download, probe, transcode, verify, publish,
// fenced finish. Output keys are a function of the video alone, but not the bytes under them:
// x264 with a VBV and frame threads differs run to run even at one thread count. A rerun, or a
// zombie that wrote before it noticed its lost lease, still leaves a rendition that plays:
// every run puts its keyframes at the same times, each segment starts on one, the init
// segments and playlists match, and the master is written last.
class JobRunner {
public:
    JobRunner(JobDeps deps, JobSettings settings);

    // `shutdown` abandons a job still downloading or transcoding and gives it back to the
    // queue; one already publishing is finished.
    [[nodiscard]] JobOutcome run(const core::ports::ClaimedJob& job,
                                 const std::stop_token& shutdown);

private:
    class Attempt;

    JobDeps deps_;
    JobSettings settings_;
};

} // namespace worker
