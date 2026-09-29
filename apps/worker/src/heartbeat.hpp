#pragma once

#include <filesystem>

namespace worker {

// A file whose modification time says the worker is making progress. The job loop touches it
// on every pass and the lease keeper on every beat, so it stays fresh while the worker idles
// and while a job runs; the pod's liveness probe restarts a worker whose file went stale.
class Heartbeat {
public:
    explicit Heartbeat(std::filesystem::path file) : file_(std::move(file)) {}

    // Creates the file the first time. A failure is left for the probe to notice.
    void beat() const noexcept;
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path file_;
};

} // namespace worker
