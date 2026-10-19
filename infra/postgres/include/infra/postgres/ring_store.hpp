#pragma once

#include "core/ports/ring_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace infra::postgres {

struct RingStoreConfig {
    std::string conninfo;
    // A ring's start and end, a room's takeover and a socket's upgrade are each one statement.
    std::size_t connections = 2;
    core::Millis connect_timeout{5000};
    // Past 2 s the ring has moved on without the row, which is best effort.
    core::Millis request_timeout{2000};
};

// The rooms' rings on Postgres (ADR-0129, migration 0026), driven by the reactor. Time is the
// database's: a ring counts until expires_at by now() on the server.
//
// The offload pool resolves host names and must be stopped before this is destroyed. Calls
// still outstanding at destruction are dropped without their callbacks.
class PgRingStore final : public core::ports::IRingStore {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    [[nodiscard]] static std::expected<std::unique_ptr<PgRingStore>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const RingStoreConfig& config);

    // Only create() can make the token.
    PgRingStore(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgRingStore() override;
    PgRingStore(const PgRingStore&) = delete;
    PgRingStore& operator=(const PgRingStore&) = delete;
    PgRingStore(PgRingStore&&) = delete;
    PgRingStore& operator=(PgRingStore&&) = delete;

    void put_ring(core::ports::StoredRing ring, core::ports::RingStoreCallback<void> done) override;
    void end_ring(const core::RoomId& room, const core::Uuid& call,
                  core::ports::RingStoreCallback<void> done) override;
    void
    ring_of(const core::RoomId& room,
            core::ports::RingStoreCallback<std::optional<core::ports::StoredRing>> done) override;
    void
    rings_for(const core::UserId& user,
              core::ports::RingStoreCallback<std::vector<core::ports::StoredRing>> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
