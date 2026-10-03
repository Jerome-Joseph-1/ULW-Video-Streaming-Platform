#include "infra/postgres/message_store.hpp"

#include "message_sql.hpp"
#include "operation.hpp"
#include "pool.hpp"
#include "result.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
           core::ports::Recording recording, MessageCallback<core::ports::Admission> done)
        : room_(room), user_(user), asked_(asked), recording_(recording), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = message_sql::kAdmits,
                         .params = Params{}
                                       .add_uuid(room_.uuid())
                                       .add_text(user_.view())
                                       .add_text(kind_text(asked_))
                                       .add_bool(recording_ == core::ports::Recording::Allowed)};
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
        // No kind: the join asked for the open kind of a room with none recorded, or for a closed
        // kind it was not to record, which it is answered as.
        if (!text) {
            done_(core::ports::admits_anyone(asked_)
                      ? core::ports::Admission::NotLive
                      : core::ports::admission(asked_, asked_, *member));
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
    core::ports::Recording recording_;
    MessageCallback<core::ports::Admission> done_;
};

// Binds the user id, which must outlive the statement.
class Access final : public Operation {
public:
    Access(const core::RoomId& room, const core::UserId& user,
           MessageCallback<core::ports::RoomAccess> done)
        : room_(room), user_(user), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = message_sql::kAccess,
                         .params = Params{}.add_uuid(room_.uuid()).add_text(user_.view())};
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
        core::ports::RoomAccess access{.kind = std::nullopt, .member = *member};
        if (text) {
            access.kind = kind_of(*text);
            if (!access.kind) {
                done_(std::unexpected(MessageStoreError::Corrupt));
                return std::nullopt;
            }
        }
        done_(access);
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    core::RoomId room_;
    core::UserId user_;
    MessageCallback<core::ports::RoomAccess> done_;
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

using core::ports::MembershipChange;
using core::ports::MembershipOutcome;

[[nodiscard]] std::optional<core::ports::MemberRole> role_of(std::string_view text) noexcept {
    if (text == "admin") {
        return core::ports::MemberRole::Admin;
    }
    if (text == "member") {
        return core::ports::MemberRole::Member;
    }
    return std::nullopt;
}

// The ids of a string_agg(user_id, ' '): none for NULL, nullopt when one is not a user id.
[[nodiscard]] std::optional<std::vector<core::UserId>>
ids_of(std::optional<std::string_view> text) {
    std::vector<core::UserId> ids;
    std::string_view rest = text.value_or("");
    while (!rest.empty()) {
        const std::size_t space = rest.find(' ');
        const auto user = core::UserId::parse(rest.substr(0, space));
        if (!user) {
            return std::nullopt;
        }
        ids.push_back(*user);
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    }
    return ids;
}

// Users as the statements take them: ids separated by spaces, which no id contains.
[[nodiscard]] std::string joined_ids(std::span<const core::UserId> users) {
    std::string out;
    for (const core::UserId& user : users) {
        if (!out.empty()) {
            out += ' ';
        }
        out += user.view();
    }
    return out;
}

[[nodiscard]] MembershipChange refused(MembershipOutcome outcome) {
    return MembershipChange{.outcome = outcome, .changed = {}, .promoted = std::nullopt};
}

// A change of one room's member list (message_sql.hpp): BEGIN, the room recorded if the change
// may create it, its row locked, the change's statement, COMMIT. The answer goes out once the
// commit succeeds, so a client never hears of a change that did not happen. A refusal decided
// from the room's kind alone answers at once and leaves the transaction to the pool, which rolls
// it back.
class LockedChange : public Operation {
public:
    // `ensure`: the kind to record the room as when nothing has, for a change that may create
    // it, made by `creator`, whose own lock it takes first.
    LockedChange(const core::RoomId& room, std::optional<core::ports::RoomKind> ensure,
                 std::optional<core::UserId> creator,
                 MessageCallback<MembershipChange> done) noexcept
        : room_(room), ensure_(ensure), creator_(creator), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept final {
        phase_ = Phase::Begin;
        answer_.reset();
        return Statement{.sql = "BEGIN", .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept final {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
            return std::nullopt;
        }
        switch (phase_) {
        case Phase::Begin:
            if (creator_) {
                phase_ = Phase::LockCreator;
                return Statement{.sql = message_sql::kLockCreator,
                                 .params = Params{}.add_text(creator_->view())};
            }
            return ensure();
        case Phase::LockCreator:
            return ensure();
        case Phase::Ensure:
            return lock();
        case Phase::Lock:
            return locked(*outcome);
        case Phase::Work: {
            auto answer = decode(*outcome);
            if (!answer) {
                done_(std::unexpected(MessageStoreError::Corrupt));
                return std::nullopt;
            }
            answer_ = std::move(*answer);
            phase_ = Phase::Commit;
            return Statement{.sql = "COMMIT", .params = {}};
        }
        case Phase::Commit:
            if (answer_) {
                done_(std::move(*answer_));
            } else {
                done_(std::unexpected(MessageStoreError::Corrupt));
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept final {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

protected:
    // What to run once the room is locked, given its recorded kind (nullopt: none is), or the
    // answer without running anything.
    using Step = std::variant<Statement, MembershipChange>;
    [[nodiscard]] virtual Step work(std::optional<core::ports::RoomKind> kind) noexcept = 0;
    // The answer, from what the change's statement returned; nullopt when it cannot be read.
    [[nodiscard]] virtual std::optional<MembershipChange> decode(const Result& r) noexcept = 0;

    [[nodiscard]] const core::RoomId& room() const noexcept { return room_; }

private:
    enum class Phase : std::uint8_t { Begin, LockCreator, Ensure, Lock, Work, Commit };

    [[nodiscard]] Statement ensure() noexcept {
        if (!ensure_) {
            return lock();
        }
        phase_ = Phase::Ensure;
        return Statement{.sql = message_sql::kEnsureRoom,
                         .params = Params{}.add_uuid(room_.uuid()).add_text(kind_text(*ensure_))};
    }

    [[nodiscard]] Statement lock() noexcept {
        phase_ = Phase::Lock;
        return Statement{.sql = message_sql::kLockRoom, .params = Params{}.add_uuid(room_.uuid())};
    }

    [[nodiscard]] std::optional<Statement> locked(const Result& r) noexcept {
        std::optional<core::ports::RoomKind> kind;
        if (r.rows() > 0) {
            kind = r.get(0, 0).and_then(kind_of);
            if (!kind) {
                done_(std::unexpected(MessageStoreError::Corrupt));
                return std::nullopt;
            }
        } else if (ensure_) {
            // Recorded a moment ago, and forgotten by the reaper before the lock: ask again.
            done_(std::unexpected(MessageStoreError::Unavailable));
            return std::nullopt;
        }
        Step step = work(kind);
        if (auto* answer = std::get_if<MembershipChange>(&step)) {
            done_(std::move(*answer));
            return std::nullopt;
        }
        phase_ = Phase::Work;
        return std::get<Statement>(std::move(step));
    }

    core::RoomId room_;
    std::optional<core::ports::RoomKind> ensure_;
    std::optional<core::UserId> creator_;
    MessageCallback<MembershipChange> done_;
    Phase phase_ = Phase::Begin;
    std::optional<MembershipChange> answer_;
};

// Who is listed now and who was added, as kOpenDirect and kCreateGroup answer, or why nobody was:
// a room that lists nobody yet stays so when it holds messages, or its creator has no room for
// one more.
[[nodiscard]] std::optional<MembershipChange> listed_and_added(const Result& r) {
    const auto listed = r.get(0, 0).and_then(parse_bool);
    auto added = ids_of(r.get(0, 1));
    const auto empty = r.get(0, 2).and_then(parse_bool);
    const auto fits = r.get(0, 3).and_then(parse_bool);
    const auto used = r.get(0, 4).and_then(parse_bool);
    if (!listed || !added || !empty || !fits || !used) {
        return std::nullopt;
    }
    if (*listed) {
        return MembershipChange{.outcome = MembershipOutcome::Done,
                                .changed = std::move(*added),
                                .promoted = std::nullopt};
    }
    if (*empty && *used) {
        return refused(MembershipOutcome::Gone);
    }
    if (*empty && !*fits) {
        return refused(MembershipOutcome::RoomLimit);
    }
    return refused(MembershipOutcome::NotMember);
}

class OpenDirect final : public LockedChange {
public:
    OpenDirect(const core::RoomId& room, const core::UserId& user, const core::UserId& peer,
               MessageCallback<MembershipChange> done) noexcept
        : LockedChange(room, core::ports::RoomKind::DirectChat, user, std::move(done)), user_(user),
          peer_(peer) {}

private:
    [[nodiscard]] Step work(std::optional<core::ports::RoomKind> kind) noexcept override {
        if (kind != core::ports::RoomKind::DirectChat) {
            return refused(MembershipOutcome::WrongKind);
        }
        return Statement{.sql = message_sql::kOpenDirect,
                         .params = Params{}
                                       .add_uuid(room().uuid())
                                       .add_text(user_.view())
                                       .add_text(peer_.view())
                                       .add_int(as_int(core::ports::kMaxRoomsPerUser))};
    }
    [[nodiscard]] std::optional<MembershipChange> decode(const Result& r) noexcept override {
        try {
            return listed_and_added(r);
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    core::UserId user_;
    core::UserId peer_;
};

class CreateGroup final : public LockedChange {
public:
    CreateGroup(const core::RoomId& room, const core::UserId& creator, std::string members,
                MessageCallback<MembershipChange> done) noexcept
        : LockedChange(room, core::ports::RoomKind::GroupChat, creator, std::move(done)),
          creator_(creator), members_(std::move(members)) {}

private:
    [[nodiscard]] Step work(std::optional<core::ports::RoomKind> kind) noexcept override {
        if (kind != core::ports::RoomKind::GroupChat) {
            return refused(MembershipOutcome::WrongKind);
        }
        return Statement{.sql = message_sql::kCreateGroup,
                         .params = Params{}
                                       .add_uuid(room().uuid())
                                       .add_text(creator_.view())
                                       .add_text(members_)
                                       .add_int(as_int(core::ports::kMaxRoomsPerUser))};
    }
    [[nodiscard]] std::optional<MembershipChange> decode(const Result& r) noexcept override {
        try {
            return listed_and_added(r);
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    core::UserId creator_;
    std::string members_;
};

// What a change by an admin answers: refused by the actor's role (NULL: not listed) and the
// room's kind, in that order, so that someone not listed learns nothing of the room.
[[nodiscard]] std::optional<MembershipOutcome>
admin_refusal(std::optional<std::string_view> role_text, bool group) noexcept {
    if (!role_text) {
        return MembershipOutcome::NotMember;
    }
    const auto role = role_of(*role_text);
    if (!role) {
        return std::nullopt;
    }
    if (!group) {
        return MembershipOutcome::NotGroup;
    }
    if (*role != core::ports::MemberRole::Admin) {
        return MembershipOutcome::NotAdmin;
    }
    return MembershipOutcome::Done;
}

class AddMembers final : public LockedChange {
public:
    AddMembers(const core::RoomId& room, const core::UserId& actor, std::string users,
               MessageCallback<MembershipChange> done) noexcept
        : LockedChange(room, std::nullopt, std::nullopt, std::move(done)), actor_(actor),
          users_(std::move(users)) {}

private:
    [[nodiscard]] Step work(std::optional<core::ports::RoomKind> kind) noexcept override {
        if (!kind) {
            return refused(MembershipOutcome::NotMember);
        }
        group_ = *kind == core::ports::RoomKind::GroupChat;
        return Statement{.sql = message_sql::kAddMembers,
                         .params = Params{}
                                       .add_uuid(room().uuid())
                                       .add_text(actor_.view())
                                       .add_text(users_)
                                       .add_int(as_int(core::ports::kMaxGroupMembers))
                                       .add_bool(group_)};
    }
    [[nodiscard]] std::optional<MembershipChange> decode(const Result& r) noexcept override {
        try {
            const auto refusal = admin_refusal(r.get(0, 0), group_);
            const auto fits = r.get(0, 1).and_then(parse_bool);
            auto added = ids_of(r.get(0, 2));
            if (!refusal || !fits || !added) {
                return std::nullopt;
            }
            if (*refusal != MembershipOutcome::Done) {
                return refused(*refusal);
            }
            if (!*fits) {
                return refused(MembershipOutcome::Full);
            }
            return MembershipChange{.outcome = MembershipOutcome::Done,
                                    .changed = std::move(*added),
                                    .promoted = std::nullopt};
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    core::UserId actor_;
    std::string users_;
    bool group_ = false;
};

class Expel final : public LockedChange {
public:
    Expel(const core::RoomId& room, const core::UserId& actor, const core::UserId& user,
          MessageCallback<MembershipChange> done) noexcept
        : LockedChange(room, std::nullopt, std::nullopt, std::move(done)), actor_(actor),
          user_(user) {}

private:
    [[nodiscard]] Step work(std::optional<core::ports::RoomKind> kind) noexcept override {
        if (!kind) {
            return refused(MembershipOutcome::NotMember);
        }
        group_ = *kind == core::ports::RoomKind::GroupChat;
        return Statement{.sql = message_sql::kExpel,
                         .params = Params{}
                                       .add_uuid(room().uuid())
                                       .add_text(actor_.view())
                                       .add_text(user_.view())
                                       .add_bool(group_)};
    }
    [[nodiscard]] std::optional<MembershipChange> decode(const Result& r) noexcept override {
        try {
            const auto refusal = admin_refusal(r.get(0, 0), group_);
            if (!refusal) {
                return std::nullopt;
            }
            if (*refusal != MembershipOutcome::Done) {
                return refused(*refusal);
            }
            auto gone = ids_of(r.get(0, 1));
            if (!gone) {
                return std::nullopt;
            }
            return MembershipChange{.outcome = MembershipOutcome::Done,
                                    .changed = std::move(*gone),
                                    .promoted = std::nullopt};
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    core::UserId actor_;
    core::UserId user_;
    bool group_ = false;
};

class Leave final : public LockedChange {
public:
    Leave(const core::RoomId& room, const core::UserId& user,
          MessageCallback<MembershipChange> done) noexcept
        : LockedChange(room, std::nullopt, std::nullopt, std::move(done)), user_(user) {}

private:
    [[nodiscard]] Step work(std::optional<core::ports::RoomKind> kind) noexcept override {
        if (!kind) {
            return refused(MembershipOutcome::NotMember);
        }
        group_ = *kind == core::ports::RoomKind::GroupChat;
        return Statement{
            .sql = message_sql::kLeave,
            .params = Params{}.add_uuid(room().uuid()).add_text(user_.view()).add_bool(group_)};
    }
    [[nodiscard]] std::optional<MembershipChange> decode(const Result& r) noexcept override {
        try {
            const auto listed = r.get(0, 0).and_then(parse_bool);
            const auto promoted = r.get(0, 1);
            if (!listed) {
                return std::nullopt;
            }
            if (!*listed) {
                return refused(MembershipOutcome::NotMember);
            }
            if (!group_) {
                return refused(MembershipOutcome::NotGroup);
            }
            MembershipChange change{
                .outcome = MembershipOutcome::Done, .changed = {user_}, .promoted = std::nullopt};
            if (promoted) {
                const auto heir = core::UserId::parse(*promoted);
                if (!heir) {
                    return std::nullopt;
                }
                change.promoted = *heir;
            }
            return change;
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    core::UserId user_;
    bool group_ = false;
};

[[nodiscard]] MessageResult<std::vector<core::ports::RoomEntry>> decode_rooms(const Result& r) {
    std::vector<core::ports::RoomEntry> rooms;
    rooms.reserve(static_cast<std::size_t>(r.rows()));
    for (int row = 0; row < r.rows(); ++row) {
        const auto room = domain_at<core::RoomId>(r, row, 0);
        const auto kind_text = r.get(row, 1);
        const auto role = r.get(row, 2).and_then(role_of);
        const auto peer_text = r.get(row, 3);
        if (!room || !role) {
            return std::unexpected(MessageStoreError::Corrupt);
        }
        core::ports::RoomEntry entry{.room = *room,
                                     .kind = core::ports::RoomKind::GroupChat,
                                     .role = *role,
                                     .peer = std::nullopt};
        if (kind_text) {
            const auto kind = kind_of(*kind_text);
            if (!kind) {
                return std::unexpected(MessageStoreError::Corrupt);
            }
            entry.kind = *kind;
        }
        if (peer_text) {
            const auto peer = core::UserId::parse(*peer_text);
            if (!peer) {
                return std::unexpected(MessageStoreError::Corrupt);
            }
            entry.peer = *peer;
        }
        rooms.push_back(entry);
    }
    return rooms;
}

// Binds the user and the cursor, which must outlive the statement.
class RoomsOf final : public Operation {
public:
    RoomsOf(const core::UserId& user, std::optional<core::RoomId> after, std::size_t limit,
            MessageCallback<std::vector<core::ports::RoomEntry>> done)
        : user_(user), after_(after), limit_(std::min(limit, core::ports::kMaxListPage + 1)),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        Params params = Params{}.add_text(user_.view()).add_int(as_int(limit_));
        if (after_) {
            return Statement{.sql = message_sql::kRoomsAfter,
                             .params = params.add_uuid(after_->uuid())};
        }
        return Statement{.sql = message_sql::kRoomsFirst, .params = params};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
            return std::nullopt;
        }
        try {
            done_(decode_rooms(*outcome));
        } catch (const std::bad_alloc&) {
            done_(std::unexpected(MessageStoreError::Unavailable));
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    core::UserId user_;
    std::optional<core::RoomId> after_;
    std::size_t limit_;
    MessageCallback<std::vector<core::ports::RoomEntry>> done_;
};

[[nodiscard]] MessageResult<core::ports::Roster> decode_roster(const Result& r) {
    core::ports::Roster roster;
    roster.members.reserve(static_cast<std::size_t>(r.rows()));
    for (int row = 0; row < r.rows(); ++row) {
        const auto listed = r.get(row, 0).and_then(parse_bool);
        if (!listed) {
            return std::unexpected(MessageStoreError::Corrupt);
        }
        roster.asker_listed = *listed;
        // The row of NULLs beside the flag: nobody on this page.
        if (!r.get(row, 1)) {
            continue;
        }
        const auto user = domain_at<core::UserId>(r, row, 1);
        const auto role = r.get(row, 2).and_then(role_of);
        if (!user || !role) {
            return std::unexpected(MessageStoreError::Corrupt);
        }
        roster.members.push_back(core::ports::MemberEntry{.user = *user, .role = *role});
    }
    return roster;
}

// Binds the asker and the cursor, which must outlive the statement.
class RosterRead final : public Operation {
public:
    RosterRead(const core::RoomId& room, const core::UserId& asker,
               std::optional<core::UserId> after, std::size_t limit,
               MessageCallback<core::ports::Roster> done)
        : room_(room), asker_(asker), after_(after),
          limit_(std::min(limit, core::ports::kMaxListPage + 1)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = message_sql::kRoster,
                         .params = Params{}
                                       .add_uuid(room_.uuid())
                                       .add_text(asker_.view())
                                       .add_text(after_ ? after_->view() : std::string_view{})
                                       .add_int(as_int(limit_))};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(MessageStoreError::Unavailable));
            return std::nullopt;
        }
        try {
            done_(decode_roster(*outcome));
        } catch (const std::bad_alloc&) {
            done_(std::unexpected(MessageStoreError::Unavailable));
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(MessageStoreError::Unavailable));
    }

private:
    core::RoomId room_;
    core::UserId asker_;
    std::optional<core::UserId> after_;
    std::size_t limit_;
    MessageCallback<core::ports::Roster> done_;
};

} // namespace

namespace {

struct MemberNotice {
    enum class Change : std::uint8_t { Added, Removed, Role };
    Change change = Change::Added;
    core::RoomId room;
    core::UserId user;
    core::ports::MemberRole role = core::ports::MemberRole::Member;
};

// "+ <room> <user>", "- <room> <user>" or "* <room> <role> <user>", as notify_chat_members()
// (migration 0014) writes it.
std::optional<MemberNotice> parse_notice(std::string_view payload) {
    if (payload.size() < 2 || payload[1] != ' ') {
        return std::nullopt;
    }
    MemberNotice::Change change = MemberNotice::Change::Added;
    if (payload[0] == '-') {
        change = MemberNotice::Change::Removed;
    } else if (payload[0] == '*') {
        change = MemberNotice::Change::Role;
    } else if (payload[0] != '+') {
        return std::nullopt;
    }
    core::ports::MemberRole role = core::ports::MemberRole::Member;
    payload.remove_prefix(2);
    const std::size_t space = payload.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const auto room = core::RoomId::parse(payload.substr(0, space));
    payload.remove_prefix(space + 1);
    if (change == MemberNotice::Change::Role) {
        const std::size_t role_end = payload.find(' ');
        const auto named = role_of(payload.substr(0, role_end));
        if (role_end == std::string_view::npos || !named) {
            return std::nullopt;
        }
        role = *named;
        payload.remove_prefix(role_end + 1);
    }
    const auto user = core::UserId::parse(payload);
    if (!room || !user) {
        return std::nullopt;
    }
    return MemberNotice{.change = change, .room = *room, .user = *user, .role = role};
}

} // namespace

class PgMessageStore::Impl final : public INotificationSink {
public:
    explicit Impl(std::unique_ptr<Pool> pool) noexcept : pool_(std::move(pool)) {}

    // The listening pool is made after this, since it points here.
    void listen_on(std::unique_ptr<Pool> listening) noexcept { listening_ = std::move(listening); }
    [[nodiscard]] Pool& pool() noexcept { return *pool_; }
    void watch(core::ports::IMemberListener* listener) noexcept { listener_ = listener; }

    void on_listening() noexcept override {
        if (listener_ != nullptr) {
            listener_->on_members_resync();
        }
    }

    void on_notification(std::string_view payload) noexcept override {
        if (listener_ == nullptr) {
            return;
        }
        // Only the trigger writes to the channel; what it cannot have written leaves every
        // list in doubt.
        const auto notice = parse_notice(payload);
        if (!notice) {
            listener_->on_members_resync();
            return;
        }
        switch (notice->change) {
        case MemberNotice::Change::Added:
            listener_->on_member_added(notice->room, notice->user);
            return;
        case MemberNotice::Change::Removed:
            listener_->on_member_removed(notice->room, notice->user);
            return;
        case MemberNotice::Change::Role:
            listener_->on_member_role(notice->room, notice->user, notice->role);
            return;
        }
    }

private:
    core::ports::IMemberListener* listener_ = nullptr;
    std::unique_ptr<Pool> pool_;
    // Last: it calls into this object, and must stop before the members above go.
    std::unique_ptr<Pool> listening_;
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
    auto impl = std::make_unique<Impl>(std::move(*pool));
    auto listening = Pool::create(reactor, offload,
                                  PoolConfig{.conninfo = config.conninfo,
                                             .application_name = "ulw-messages-listen",
                                             .connections = 1,
                                             .connect_timeout = config.connect_timeout,
                                             .request_timeout = config.request_timeout,
                                             .listen = "LISTEN chat_members",
                                             .notifications = impl.get()});
    if (!listening) {
        return std::unexpected(std::move(listening.error()));
    }
    impl->listen_on(std::move(*listening));
    return std::make_unique<PgMessageStore>(Token{}, std::move(impl));
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
                            core::ports::RoomKind asked, core::ports::Recording recording,
                            MessageCallback<core::ports::Admission> done) {
    impl_->pool().submit(std::make_unique<Admits>(room, user, asked, recording, std::move(done)));
}

void PgMessageStore::access(const core::RoomId& room, const core::UserId& user,
                            MessageCallback<core::ports::RoomAccess> done) {
    impl_->pool().submit(std::make_unique<Access>(room, user, std::move(done)));
}

void PgMessageStore::record_live(const core::RoomId& room, MessageCallback<void> done) {
    impl_->pool().submit(std::make_unique<RecordLive>(room, std::move(done)));
}

void PgMessageStore::watch_members(core::ports::IMemberListener* listener) noexcept {
    impl_->watch(listener);
}

void PgMessageStore::open_direct(const core::RoomId& room, const core::UserId& user,
                                 const core::UserId& peer, MessageCallback<MembershipChange> done) {
    impl_->pool().submit(std::make_unique<OpenDirect>(room, user, peer, std::move(done)));
}

void PgMessageStore::create_group(const core::RoomId& room, const core::UserId& creator,
                                  std::vector<core::UserId> members,
                                  MessageCallback<MembershipChange> done) {
    impl_->pool().submit(
        std::make_unique<CreateGroup>(room, creator, joined_ids(members), std::move(done)));
}

void PgMessageStore::add_members(const core::RoomId& room, const core::UserId& actor,
                                 std::vector<core::UserId> users,
                                 MessageCallback<MembershipChange> done) {
    impl_->pool().submit(
        std::make_unique<AddMembers>(room, actor, joined_ids(users), std::move(done)));
}

void PgMessageStore::expel(const core::RoomId& room, const core::UserId& actor,
                           const core::UserId& user, MessageCallback<MembershipChange> done) {
    if (actor == user) {
        leave_room(room, user, std::move(done));
        return;
    }
    impl_->pool().submit(std::make_unique<Expel>(room, actor, user, std::move(done)));
}

void PgMessageStore::leave_room(const core::RoomId& room, const core::UserId& user,
                                MessageCallback<MembershipChange> done) {
    impl_->pool().submit(std::make_unique<Leave>(room, user, std::move(done)));
}

void PgMessageStore::rooms_of(const core::UserId& user, std::optional<core::RoomId> after,
                              std::size_t limit,
                              MessageCallback<std::vector<core::ports::RoomEntry>> done) {
    impl_->pool().submit(std::make_unique<RoomsOf>(user, after, limit, std::move(done)));
}

void PgMessageStore::roster(const core::RoomId& room, const core::UserId& asker,
                            std::optional<core::UserId> after, std::size_t limit,
                            MessageCallback<core::ports::Roster> done) {
    impl_->pool().submit(std::make_unique<RosterRead>(room, asker, after, limit, std::move(done)));
}

} // namespace infra::postgres
