#include "infra/postgres/message_store.hpp"

#include "message_sql.hpp"
#include "operation.hpp"
#include "pool.hpp"
#include "result.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>

namespace infra::postgres {

namespace {

using core::ports::kMaxHistoryBytes;
using core::ports::kMaxHistoryRows;
using core::ports::kMaxMembersPage;
using core::ports::MessageCallback;
using core::ports::MessageResult;
using core::ports::MessageStoreError;
using core::ports::StoredMessage;

// Seqs are room_state.last_seq values, bigint, from 1; row limits and byte bounds are small.
// Only the cursor of a first page, "below anything", is clamped.
std::int64_t as_int(std::uint64_t n) noexcept {
    return static_cast<std::int64_t>(
        std::min<std::uint64_t>(n, std::numeric_limits<std::int64_t>::max()));
}

// One statement that binds nothing borrowed, and whose outcome `decode` turns into the answer.
template <class T> class Single final : public Operation {
public:
    using Decode = MessageResult<T> (*)(const Result&);

    Single(Statement statement, Decode decode, MessageCallback<T> done) noexcept
        : statement_(statement), decode_(decode), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override { return statement_; }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
        } else {
            done_(decode_(*outcome));
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    Statement statement_;
    Decode decode_;
    MessageCallback<T> done_;
};

[[nodiscard]] MessageResult<std::vector<StoredMessage>> decode_page(const Result& r) {
    std::vector<StoredMessage> page;
    page.reserve(static_cast<std::size_t>(r.rows()));
    for (int row = 0; row < r.rows(); ++row) {
        const auto seq = r.get(row, 0).and_then(parse_uint64);
        const auto sender = domain_at<core::UserId>(r, row, 1);
        const auto key = r.get(row, 2);
        const auto micros = r.get(row, 3).and_then(parse_int64);
        auto body = r.get(row, 4).and_then(parse_bytea);
        if (!seq || !sender || !key || !micros || !body) {
            return std::unexpected(MessageStoreError::Corrupt);
        }
        page.push_back(StoredMessage{
            .seq = *seq,
            .sender = *sender,
            .key = std::string{*key},
            .sent_at = core::WallTime{std::chrono::microseconds{*micros}},
            .body = std::move(*body),
        });
    }
    return page;
}

[[nodiscard]] MessageResult<std::uint64_t> decode_last_seq(const Result& r) {
    const auto seq = r.get(0, 0).and_then(parse_uint64);
    if (!seq) {
        return std::unexpected(MessageStoreError::Corrupt);
    }
    return *seq;
}

[[nodiscard]] MessageResult<std::vector<core::UserId>> decode_members(const Result& r) {
    std::vector<core::UserId> members;
    members.reserve(static_cast<std::size_t>(r.rows()));
    for (int row = 0; row < r.rows(); ++row) {
        const auto user = domain_at<core::UserId>(r, row, 0);
        if (!user) {
            return std::unexpected(MessageStoreError::Corrupt);
        }
        members.push_back(*user);
    }
    return members;
}

// The members call binds the id it pages after; it has to outlive the statement.
class Members final : public Operation {
public:
    Members(const core::RoomId& room, std::optional<core::UserId> after, std::size_t limit,
            MessageCallback<std::vector<core::UserId>> done)
        : room_(room), after_(after), limit_(std::min(limit, kMaxMembersPage)),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = message_sql::kMembers,
                         .params = Params{}
                                       .add_uuid(room_.uuid())
                                       .add_text(after_ ? after_->view() : std::string_view{})
                                       .add_int(as_int(limit_))};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
        } else {
            done_(decode_members(*outcome));
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    core::RoomId room_;
    std::optional<core::UserId> after_;
    std::size_t limit_;
    MessageCallback<std::vector<core::UserId>> done_;
};

// Membership writes bind the user id, which must outlive the statement as well.
class MemberWrite final : public Operation {
public:
    MemberWrite(Sql sql, const core::RoomId& room, const core::UserId& user,
                MessageCallback<void> done)
        : sql_(sql), room_(room), user_(user), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = sql_,
                         .params = Params{}.add_uuid(room_.uuid()).add_text(user_.view())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
        } else {
            done_({});
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    Sql sql_;
    core::RoomId room_;
    core::UserId user_;
    MessageCallback<void> done_;
};

// The names section 8.15 gives the kinds, as chat_rooms.kind holds them.
[[nodiscard]] std::string_view kind_text(core::ports::RoomKind kind) noexcept {
    switch (kind) {
    case core::ports::RoomKind::DirectChat:
        return "direct_chat";
    case core::ports::RoomKind::GroupChat:
        return "group_chat";
    case core::ports::RoomKind::StreamLiveChat:
        return "stream_live_chat";
    }
    return "group_chat";
}

[[nodiscard]] std::optional<core::ports::RoomKind> kind_of(std::string_view text) noexcept {
    for (const auto kind : {core::ports::RoomKind::DirectChat, core::ports::RoomKind::GroupChat,
                            core::ports::RoomKind::StreamLiveChat}) {
        if (kind_text(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

// Binds the user id, which must outlive the statement.
class Admits final : public Operation {
public:
    Admits(const core::RoomId& room, const core::UserId& user, core::ports::RoomKind asked,
           MessageCallback<core::ports::Admission> done)
        : room_(room), user_(user), asked_(asked), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{
            .sql = message_sql::kAdmits,
            .params =
                Params{}.add_uuid(room_.uuid()).add_text(user_.view()).add_text(kind_text(asked_))};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
            return std::nullopt;
        }
        const auto text = outcome->get(0, 0);
        const auto member = outcome->get(0, 1).and_then(parse_bool);
        if (!member) {
            done_(std::unexpected(MessageStoreError::Corrupt));
            return std::nullopt;
        }
        // No kind: the join asked for the open kind of a room with none recorded.
        if (!text) {
            done_(core::ports::Admission::NotLive);
            return std::nullopt;
        }
        const auto kind = kind_of(*text);
        if (!kind) {
            done_(std::unexpected(MessageStoreError::Corrupt));
            return std::nullopt;
        }
        done_(core::ports::admission(asked_, *kind, *member));
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    core::RoomId room_;
    core::UserId user_;
    core::ports::RoomKind asked_;
    MessageCallback<core::ports::Admission> done_;
};

class RecordLive final : public Operation {
public:
    RecordLive(const core::RoomId& room, MessageCallback<void> done)
        : room_(room), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = message_sql::kRecordLive,
                         .params = Params{}.add_uuid(room_.uuid())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
            return std::nullopt;
        }
        // No row: no kind recorded, and the room lists members or was created as another kind.
        if (outcome->rows() == 0) {
            done_(std::unexpected(MessageStoreError::Conflict));
            return std::nullopt;
        }
        const auto kind = outcome->get(0, 0).and_then(kind_of);
        if (!kind) {
            done_(std::unexpected(MessageStoreError::Corrupt));
        } else if (*kind != core::ports::RoomKind::StreamLiveChat) {
            done_(std::unexpected(MessageStoreError::Conflict));
        } else {
            done_({});
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    core::RoomId room_;
    MessageCallback<void> done_;
};

} // namespace

class PgMessageStore::Impl {
public:
    explicit Impl(std::unique_ptr<Pool> pool) noexcept : pool_(std::move(pool)) {}

    [[nodiscard]] Pool& pool() noexcept { return *pool_; }

private:
    std::unique_ptr<Pool> pool_;
};

std::expected<std::unique_ptr<PgMessageStore>, std::string>
PgMessageStore::create(net::IReactor& reactor, net::OffloadPool& offload,
                       const MessageStoreConfig& config) {
    auto pool = Pool::create(reactor, offload,
                             PoolConfig{.conninfo = config.conninfo,
                                        .application_name = "ulw-messages",
                                        .connections = config.connections,
                                        .connect_timeout = config.connect_timeout,
                                        .request_timeout = config.request_timeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    return std::make_unique<PgMessageStore>(Token{}, std::make_unique<Impl>(std::move(*pool)));
}

PgMessageStore::PgMessageStore(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgMessageStore::~PgMessageStore() = default;

void PgMessageStore::history_before(const core::RoomId& room, std::optional<std::uint64_t> before,
                                    std::size_t limit,
                                    MessageCallback<std::vector<StoredMessage>> done) {
    impl_->pool().submit(std::make_unique<Single<std::vector<StoredMessage>>>(
        Statement{.sql = message_sql::kHistoryBefore,
                  .params = Params{}
                                .add_uuid(room.uuid())
                                .add_int(as_int(
                                    before.value_or(std::numeric_limits<std::uint64_t>::max())))
                                .add_int(as_int(std::min(limit, kMaxHistoryRows)))
                                .add_int(as_int(kMaxHistoryBytes))},
        &decode_page, std::move(done)));
}

void PgMessageStore::history_after(const core::RoomId& room, std::uint64_t after, std::size_t limit,
                                   MessageCallback<std::vector<StoredMessage>> done) {
    impl_->pool().submit(std::make_unique<Single<std::vector<StoredMessage>>>(
        Statement{.sql = message_sql::kHistoryAfter,
                  .params = Params{}
                                .add_uuid(room.uuid())
                                .add_int(as_int(after))
                                .add_int(as_int(std::min(limit, kMaxHistoryRows)))
                                .add_int(as_int(kMaxHistoryBytes))},
        &decode_page, std::move(done)));
}

void PgMessageStore::last_seq(const core::RoomId& room, MessageCallback<std::uint64_t> done) {
    impl_->pool().submit(std::make_unique<Single<std::uint64_t>>(
        Statement{.sql = message_sql::kLastSeq, .params = Params{}.add_uuid(room.uuid())},
        &decode_last_seq, std::move(done)));
}

void PgMessageStore::add_member(const core::RoomId& room, const core::UserId& user,
                                MessageCallback<void> done) {
    impl_->pool().submit(
        std::make_unique<MemberWrite>(message_sql::kAddMember, room, user, std::move(done)));
}

void PgMessageStore::remove_member(const core::RoomId& room, const core::UserId& user,
                                   MessageCallback<void> done) {
    impl_->pool().submit(
        std::make_unique<MemberWrite>(message_sql::kRemoveMember, room, user, std::move(done)));
}

void PgMessageStore::members(const core::RoomId& room, std::optional<core::UserId> after,
                             std::size_t limit, MessageCallback<std::vector<core::UserId>> done) {
    impl_->pool().submit(std::make_unique<Members>(room, after, limit, std::move(done)));
}

void PgMessageStore::admits(const core::RoomId& room, const core::UserId& user,
                            core::ports::RoomKind asked,
                            MessageCallback<core::ports::Admission> done) {
    impl_->pool().submit(std::make_unique<Admits>(room, user, asked, std::move(done)));
}

void PgMessageStore::record_live(const core::RoomId& room, MessageCallback<void> done) {
    impl_->pool().submit(std::make_unique<RecordLive>(room, std::move(done)));
}

} // namespace infra::postgres
