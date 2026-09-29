#pragma once

#include "core/util/time.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace infra::postgres {

// The gateway's readiness question to the database, on a session of its own so it never
// queues behind the catalog's statements. It connects on first use and reconnects on the call
// after a failure. Every call blocks: never on a reactor thread.
class PgHealthCheck {
public:
    explicit PgHealthCheck(std::string conninfo);
    ~PgHealthCheck();
    PgHealthCheck(const PgHealthCheck&) = delete;
    PgHealthCheck& operator=(const PgHealthCheck&) = delete;
    PgHealthCheck(PgHealthCheck&&) = delete;
    PgHealthCheck& operator=(PgHealthCheck&&) = delete;

    // How long the oldest transcode job due to run has waited; nullopt when none is waiting.
    // An error, with the server's message, when the database did not answer.
    [[nodiscard]] std::expected<std::optional<core::Seconds>, std::string> oldest_queued_job();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
