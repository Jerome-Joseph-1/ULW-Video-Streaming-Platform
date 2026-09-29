#pragma once

#include "core/ports/job_queue.hpp"

#include "sqlstate.hpp"

namespace infra::postgres {

// Invalid tells the worker the same call will fail the same way every time, and it fails the
// job for good on it. Only a constraint says that for certain. Rejected also covers a server out
// of a resource (53xxx), a system error (58xxx, XX000) and a read-only transaction after a
// failover (25006), all of which pass; a statement that is itself wrong is a bug the lease and
// the reaper's attempts bound either way.
[[nodiscard]] inline core::ports::JobQueueError to_queue_error(DbError e) noexcept {
    switch (e) {
    case DbError::Duplicate:
    case DbError::Constraint:
        return core::ports::JobQueueError::Invalid;
    case DbError::Retry:
    case DbError::ConnectionLost:
    case DbError::Timeout:
    case DbError::LockTimeout:
    case DbError::Rejected:
        return core::ports::JobQueueError::Unavailable;
    }
    return core::ports::JobQueueError::Unavailable;
}

} // namespace infra::postgres
