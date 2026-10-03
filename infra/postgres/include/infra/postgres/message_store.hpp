#pragma once

#include "core/ports/message_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace infra::postgres {

struct MessageStoreConfig {
    std::string conninfo;
    // Mostly history pages, each one statement by primary key. Four, as the room store's: a
    // chat node then holds 4 + 1 (listening for member removals) + 5 sessions, and three nodes
    // 30 of Postgres' default 100.
    std::size_t connections = 4;
    core::Millis connect_timeout{5000};
    // Every statement reads or writes by primary key, at most kMaxHistoryRows rows of at most
    // kMaxHistoryBytes: milliseconds. Two seconds, as the room store's, is a server that is
    // stuck.
    core::Millis request_timeout{2000};
};

// IMessageStore on Postgres (migrations/0005_chat_messages.sql), driven by the reactor: no call
// blocks the loop. One more session LISTENs for member removals (migrations/0009). It only reads
// messages; PgRoomStore::append writes them. Bodies come back in bytea's hex text form, and neither
// they nor anything derived from them reaches a log or an error.
//
// The offload pool resolves host names and must be stopped before this is destroyed. Calls
// outstanding at destruction are dropped unanswered.
class PgMessageStore final : public core::ports::IMessageStore {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    // Refuses a connection string that does not parse, and one that would make libpq look the
    // host up itself.
    [[nodiscard]] static std::expected<std::unique_ptr<PgMessageStore>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const MessageStoreConfig& config);

    // Only create() can make the token.
    PgMessageStore(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgMessageStore() override;
    PgMessageStore(const PgMessageStore&) = delete;
    PgMessageStore& operator=(const PgMessageStore&) = delete;
    PgMessageStore(PgMessageStore&&) = delete;
    PgMessageStore& operator=(PgMessageStore&&) = delete;

    void history_before(
        const core::RoomId& room, std::optional<std::uint64_t> before, std::size_t limit,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override;
    void history_after(
        const core::RoomId& room, std::uint64_t after, std::size_t limit,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override;
    void last_seq(const core::RoomId& room,
                  core::ports::MessageCallback<std::uint64_t> done) override;
    void add_member(const core::RoomId& room, const core::UserId& user,
                    core::ports::MessageCallback<void> done) override;
    void remove_member(const core::RoomId& room, const core::UserId& user,
                       core::ports::MessageCallback<void> done) override;
    void members(const core::RoomId& room, std::optional<core::UserId> after, std::size_t limit,
                 core::ports::MessageCallback<std::vector<core::UserId>> done) override;
    using core::ports::IMessageStore::admits;
    void admits(const core::RoomId& room, const core::UserId& user, core::ports::RoomKind asked,
                core::ports::Recording recording,
                core::ports::MessageCallback<core::ports::Admission> done) override;
    void access(const core::RoomId& room, const core::UserId& user,
                core::ports::MessageCallback<core::ports::RoomAccess> done) override;
    void record_live(const core::RoomId& room, core::ports::MessageCallback<void> done) override;
    void watch_members(core::ports::IMemberListener* listener) noexcept override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
