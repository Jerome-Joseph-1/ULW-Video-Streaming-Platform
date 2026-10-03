#pragma once

#include "core/ports/live.hpp"
#include "net/reactor.hpp"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace infra::packagers {

struct ProcessConfig {
    // The live_packager binary.
    std::string binary;
    // "NAME=value" lines every packager is started with, besides the stream's own
    // ULW_STREAM_ID, ULW_STREAM_OWNER and ULW_LIVE_SRT_PASSPHRASE: its storage, database,
    // scratch directory and ingest port. Nothing of the gateway's own environment is passed on.
    std::vector<std::string> environment;
};

// Packagers as child processes of this one, for development, the local stack and the browser
// suite (ADR-0091); on a cluster each is a Job instead. A child's exit is seen through a pidfd
// the reactor watches. Children outlive this object: destroying it stops watching them, and a
// stream still running runs to its end on its own.
//
// Each child takes the ingest port its environment names, so this runs one stream at a time
// unless that is 0, which no relay could then find.
class ProcessPackagers final : public core::ports::IPackagers {
public:
    class Impl;

    [[nodiscard]] static std::expected<std::unique_ptr<ProcessPackagers>, std::string>
    create(net::IReactor& reactor, ProcessConfig config);

    explicit ProcessPackagers(std::unique_ptr<Impl> impl) noexcept;
    ~ProcessPackagers() override;
    ProcessPackagers(const ProcessPackagers&) = delete;
    ProcessPackagers& operator=(const ProcessPackagers&) = delete;
    ProcessPackagers(ProcessPackagers&&) = delete;
    ProcessPackagers& operator=(ProcessPackagers&&) = delete;

    void start(const core::ports::PackagerSpec& spec, core::ports::PackagerDone done) override;
    void state(const core::LiveStreamId& stream, core::ports::PackagerStateDone done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::packagers
