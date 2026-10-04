#pragma once

#include "core/ports/live.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace infra::postgres {

struct LiveStreamsConfig {
    std::string conninfo;
    // 1..8. A call is one or two single-row statements, a handful per stream per minute.
    std::size_t connections = 2;
    core::Millis connect_timeout{5000};
    // Every statement touches rows by key or by a small partial index; past 5 s it is stuck.
    core::Millis request_timeout{5000};
};

// ILiveStreamStore on Postgres (live_streams, migration 0011), driven by the reactor: no call
// ever blocks the loop. The offload pool resolves host names and must be stopped before this is
// destroyed; calls still outstanding then are dropped without their callbacks.
class PgLiveStreams final : public core::ports::ILiveStreamStore {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    [[nodiscard]] static std::expected<std::unique_ptr<PgLiveStreams>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const LiveStreamsConfig& config);

    // Only create() can make the token.
    PgLiveStreams(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgLiveStreams() override;
    PgLiveStreams(const PgLiveStreams&) = delete;
    PgLiveStreams& operator=(const PgLiveStreams&) = delete;
    PgLiveStreams(PgLiveStreams&&) = delete;
    PgLiveStreams& operator=(PgLiveStreams&&) = delete;

    void create(core::ports::NewLiveStream stream, core::ports::LiveLimits limits,
                core::ports::LiveCallback<core::ports::CreatedLiveStream> done) override;
    void find(const core::LiveStreamId& id,
              core::ports::LiveCallback<core::ports::LiveStream> done) override;
    void mark_live(const core::LiveStreamId& id, core::WallTime at,
                   core::ports::LiveCallback<core::ports::LiveStream> done) override;
    void end(const core::LiveStreamId& id, core::ports::LiveEnd reason, core::WallTime at,
             core::ports::LiveCallback<core::ports::EndedLiveStream> done) override;
    void unfinished(std::size_t limit,
                    core::ports::LiveCallback<std::vector<core::ports::LiveStream>> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
