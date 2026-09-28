#pragma once

#include "core/ports/job_queue.hpp"

#include "sync_connection.hpp"

namespace infra::postgres {

// Half a heartbeat interval: a beat stuck behind a lock or a dead server gives up in time for
// the next one, so one stuck statement never costs the lease (which has room for two missed
// beats). The same bound ends the session of a worker that stalls inside finish(), whose job
// and video row locks the reaper's SKIP LOCKED would otherwise pass over for as long as it
// stays stalled.
inline constexpr SessionSettings kJobSession{.application_name = "ulw-jobs",
                                             .statement_timeout = core::ports::kJobHeartbeat / 2};

} // namespace infra::postgres
