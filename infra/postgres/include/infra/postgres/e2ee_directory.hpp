#pragma once

#include "core/ports/e2ee.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace infra::postgres {

struct E2eeDirectoryConfig {
    std::string conninfo;
    // 1..32. A call holds a session for one to four sub-millisecond round trips, and a fetch
    // happens once per invitation, so 4 carry far more than a chat node asks for.
    std::size_t connections = 4;
    // As for the catalog: a handful of round trips on the private network.
    core::Millis connect_timeout{5000};
    // Every statement touches one device's rows by key. Past 5 s it is stuck behind a lock or
    // a dead server, and the client's retry is better than waiting on.
    core::Millis request_timeout{5000};
};

// The device registry and key package directory on Postgres, driven by the reactor.
//
// The single-use guarantee rests on the fetch being one statement that deletes the package it
// returns, so no reply can carry a package whose row survived. Deregistration and fetches of the
// same device are serialised on the device's row lock: a fetch either finishes before the
// device is retired or sees it retired.
//
// The offload pool resolves host names and must be stopped before this is destroyed. Calls
// still outstanding at destruction are dropped without their callbacks.
class PgE2eeDirectory final : public core::ports::IDeviceRegistry,
                              public core::ports::IE2eeDeliveryService {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    [[nodiscard]] static std::expected<std::unique_ptr<PgE2eeDirectory>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const E2eeDirectoryConfig& config);

    // Only create() can make the token.
    PgE2eeDirectory(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgE2eeDirectory() override;
    PgE2eeDirectory(const PgE2eeDirectory&) = delete;
    PgE2eeDirectory& operator=(const PgE2eeDirectory&) = delete;
    PgE2eeDirectory(PgE2eeDirectory&&) = delete;
    PgE2eeDirectory& operator=(PgE2eeDirectory&&) = delete;

    void register_device(const core::UserId& user, const core::DeviceId& device,
                         core::ports::E2eeCallback<void> done) override;
    void deregister_device(const core::UserId& user, const core::DeviceId& device,
                           core::ports::E2eeCallback<void> done) override;
    void publish_key_packages(const core::UserId& user, const core::DeviceId& device,
                              std::vector<core::ports::KeyPackageBytes> batch,
                              core::ports::E2eeCallback<std::size_t> done) override;
    void fetch_key_package(const core::UserId& user, const core::DeviceId& device,
                           core::ports::E2eeCallback<core::ports::FetchedKeyPackage> done) override;
    void submit_commit(const core::RoomId& room, const core::UserId& user,
                       const core::DeviceId& committer, std::uint64_t epoch,
                       core::ports::CommitBytes commit,
                       core::ports::E2eeCallback<void> done) override;
    void
    fetch_commits(const core::RoomId& room, std::uint64_t from_epoch,
                  core::ports::E2eeCallback<std::vector<core::ports::StoredCommit>> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
