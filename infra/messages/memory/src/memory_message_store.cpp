#include "infra/messages/memory_message_store.hpp"

#include "rt/room_store.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

namespace infra::messages {

namespace {

using core::ports::kMaxHistoryBytes;
using core::ports::kMaxHistoryRows;
using core::ports::MessageCallback;
using core::ports::MessageResult;
using core::ports::MessageStoreError;
using core::ports::StoredMessage;

template <class View> std::vector<StoredMessage> page(View messages, std::size_t limit) {
    const std::size_t rows = std::min(limit, kMaxHistoryRows);
    std::vector<StoredMessage> out;
    std::size_t bytes = 0;
    for (const StoredMessage& m : messages) {
        bytes += m.body.size();
        // The first message goes in whatever its size, as it does from the durable store.
        if (out.size() == rows || (bytes > kMaxHistoryBytes && !out.empty())) {
            break;
        }
        out.push_back(m);
    }
    return out;
}

} // namespace

MemoryMessageStore::MemoryMessageStore(net::IReactor& reactor) : reactor_(reactor) {}

MemoryMessageStore::~MemoryMessageStore() {
    reactor_.cancel_timer(timer_);
}

void MemoryMessageStore::defer(std::move_only_function<void() noexcept> fn) {
    pending_.push_back(std::move(fn));
    if (timer_ == net::TimerId{}) {
        timer_ = reactor_.arm_timer(core::Millis{0}, *this);
    }
}

void MemoryMessageStore::on_timeout() noexcept {
    timer_ = {};
    // Callbacks may issue further calls; those land in a fresh batch for the next iteration.
    std::vector<std::move_only_function<void() noexcept>> batch;
    batch.swap(pending_);
    for (auto& fn : batch) {
        fn();
    }
}

void MemoryMessageStore::append(const core::RoomId& room, std::uint64_t seq,
                                const core::UserId& sender, std::string key,
                                std::vector<std::byte> body, core::WallTime sent_at,
                                MessageCallback<std::uint64_t> done) {
    MessageResult<std::uint64_t> result = seq;
    Room& r = rooms_[room];
    std::pair<std::string, std::string> sender_key{sender.view(), key};
    if (body.size() > core::ports::kMaxMessageBody) {
        result = std::unexpected(MessageStoreError::TooLarge);
    } else if (rt::is_ephemeral_room(room)) {
        // Its seq is taken; its message is not kept.
        r.last = std::max(r.last, seq);
    } else if (const auto stored = r.keys.find(sender_key); stored != r.keys.end()) {
        // The same message again is answered with its seq; another under its key is refused.
        const bool same = r.messages.at(stored->second).body == body;
        result = same ? MessageResult<std::uint64_t>{stored->second}
                      : std::unexpected(MessageStoreError::Conflict);
    } else if (r.messages.contains(seq)) {
        result = std::unexpected(MessageStoreError::Conflict);
    } else {
        r.keys.emplace(std::move(sender_key), seq);
        r.messages.emplace(
            seq, StoredMessage{.seq = seq,
                               .sender = sender,
                               .key = std::move(key),
                               .sent_at = std::chrono::floor<std::chrono::microseconds>(sent_at),
                               .body = std::move(body)});
    }
    defer([done = std::move(done), result]() mutable noexcept { done(result); });
}

void MemoryMessageStore::history_before(const core::RoomId& room,
                                        std::optional<std::uint64_t> before, std::size_t limit,
                                        MessageCallback<std::vector<StoredMessage>> done) {
    std::vector<StoredMessage> out;
    if (const auto it = rooms_.find(room); it != rooms_.end()) {
        const auto& messages = it->second.messages;
        const auto end = before ? messages.lower_bound(*before) : messages.end();
        out = page(std::ranges::subrange(messages.begin(), end) | std::views::reverse |
                       std::views::values,
                   limit);
    }
    defer([done = std::move(done), out = std::move(out)]() mutable noexcept {
        done(std::move(out));
    });
}

void MemoryMessageStore::history_after(const core::RoomId& room, std::uint64_t after,
                                       std::size_t limit,
                                       MessageCallback<std::vector<StoredMessage>> done) {
    std::vector<StoredMessage> out;
    if (const auto it = rooms_.find(room); it != rooms_.end()) {
        const auto& messages = it->second.messages;
        out = page(std::ranges::subrange(messages.upper_bound(after), messages.end()) |
                       std::views::values,
                   limit);
    }
    defer([done = std::move(done), out = std::move(out)]() mutable noexcept {
        done(std::move(out));
    });
}

void MemoryMessageStore::last_seq(const core::RoomId& room, MessageCallback<std::uint64_t> done) {
    std::uint64_t seq = 0;
    if (const auto it = rooms_.find(room); it != rooms_.end()) {
        seq = it->second.last;
        if (!it->second.messages.empty()) {
            seq = std::max(seq, it->second.messages.rbegin()->first);
        }
    }
    defer([done = std::move(done), seq]() mutable noexcept { done(seq); });
}

void MemoryMessageStore::add_member(const core::RoomId& room, const core::UserId& user,
                                    MessageCallback<void> done) {
    kinds_.try_emplace(room, core::ports::RoomKind::GroupChat);
    const bool added = members_[room].try_emplace(user, core::ports::MemberRole::Member).second;
    defer([this, done = std::move(done), added, room, user]() mutable noexcept {
        done({});
        if (added && listener_ != nullptr) {
            listener_->on_member_added(room, user);
        }
    });
}

void MemoryMessageStore::remove_member(const core::RoomId& room, const core::UserId& user,
                                       MessageCallback<void> done) {
    bool removed = false;
    if (const auto it = members_.find(room); it != members_.end()) {
        removed = it->second.erase(user) != 0;
    }
    defer([this, done = std::move(done), removed, room, user]() mutable noexcept {
        done({});
        if (removed && listener_ != nullptr) {
            listener_->on_member_removed(room, user);
        }
    });
}

void MemoryMessageStore::members(const core::RoomId& room, std::optional<core::UserId> after,
                                 std::size_t limit,
                                 MessageCallback<std::vector<core::UserId>> done) {
    std::vector<core::UserId> out;
    if (const auto it = members_.find(room); it != members_.end()) {
        const auto& ids = it->second;
        const std::size_t rows = std::min(limit, core::ports::kMaxMembersPage);
        for (auto from = after ? ids.upper_bound(*after) : ids.begin();
             from != ids.end() && out.size() < rows; ++from) {
            out.push_back(from->first);
        }
    }
    defer([done = std::move(done), out = std::move(out)]() mutable noexcept {
        done(std::move(out));
    });
}

void MemoryMessageStore::admits(const core::RoomId& room, const core::UserId& user,
                                core::ports::RoomKind asked, core::ports::Recording recording,
                                MessageCallback<core::ports::Admission> done) {
    auto recorded = kinds_.find(room);
    core::ports::RoomKind kind = asked;
    if (recorded == kinds_.end()) {
        if (core::ports::admits_anyone(asked)) {
            defer([done = std::move(done)]() mutable noexcept {
                done(core::ports::Admission::NotLive);
            });
            return;
        }
        if (recording == core::ports::Recording::Allowed) {
            kinds_.emplace(room, asked);
        }
    } else {
        kind = recorded->second;
    }
    const auto listed = members_.find(room);
    const bool member = listed != members_.end() && listed->second.contains(user);
    const auto answer = core::ports::admission(asked, kind, member);
    defer([done = std::move(done), answer]() mutable noexcept { done(answer); });
}

void MemoryMessageStore::access(const core::RoomId& room, const core::UserId& user,
                                MessageCallback<core::ports::RoomAccess> done) {
    core::ports::RoomAccess answer;
    if (const auto recorded = kinds_.find(room); recorded != kinds_.end()) {
        answer.kind = recorded->second;
    }
    const auto listed = members_.find(room);
    answer.member = listed != members_.end() && listed->second.contains(user);
    defer([done = std::move(done), answer]() mutable noexcept { done(answer); });
}

void MemoryMessageStore::record_live(const core::RoomId& room, MessageCallback<void> done) {
    const auto listed = members_.find(room);
    const bool has_members = listed != members_.end() && !listed->second.empty();
    const auto recorded = kinds_.find(room);
    bool open = false;
    if (recorded != kinds_.end()) {
        open = recorded->second == core::ports::RoomKind::StreamLiveChat;
    } else if (!has_members && core::ports::is_stream_chat(room)) {
        kinds_.emplace(room, core::ports::RoomKind::StreamLiveChat);
        open = true;
    }
    defer([done = std::move(done), open]() mutable noexcept {
        if (open) {
            done({});
        } else {
            done(std::unexpected(core::ports::MessageStoreError::Conflict));
        }
    });
}

} // namespace infra::messages

namespace infra::messages {

using core::ports::MemberRole;
using core::ports::MembershipChange;
using core::ports::MembershipOutcome;

namespace {

MembershipChange refusal(MembershipOutcome outcome) {
    return MembershipChange{.outcome = outcome, .changed = {}, .promoted = std::nullopt};
}

} // namespace

void MemoryMessageStore::answer_change(MembershipChange change, bool added,
                                       const core::RoomId& room,
                                       MessageCallback<MembershipChange> done) {
    defer(
        [this, change = std::move(change), added, room, done = std::move(done)]() mutable noexcept {
            // The listener hears of the change after its answer, as the durable store's
            // notifications come after its commit; here the two may arrive in one iteration.
            const std::vector<core::UserId> changed = change.changed;
            done(std::move(change));
            for (const core::UserId& user : changed) {
                if (listener_ == nullptr) {
                    break;
                }
                if (added) {
                    listener_->on_member_added(room, user);
                } else {
                    listener_->on_member_removed(room, user);
                }
            }
        });
}

void MemoryMessageStore::create_listed(const core::RoomId& room, core::ports::RoomKind kind,
                                       const core::UserId& asker, std::vector<core::UserId> users,
                                       MemberRole first_role,
                                       MessageCallback<MembershipChange> done) {
    const auto recorded = kinds_.try_emplace(room, kind).first->second;
    if (recorded != kind) {
        answer_change(refusal(MembershipOutcome::WrongKind), true, room, std::move(done));
        return;
    }
    Members& listed = members_[room];
    MembershipChange change = refusal(MembershipOutcome::Done);
    if (listed.empty()) {
        for (std::size_t i = 0; i < users.size(); ++i) {
            if (listed.try_emplace(users[i], i == 0 ? first_role : MemberRole::Member).second) {
                change.changed.push_back(users[i]);
            }
        }
        std::ranges::sort(change.changed, ByteOrder{});
    } else if (!listed.contains(asker)) {
        change = refusal(MembershipOutcome::NotMember);
    }
    answer_change(std::move(change), true, room, std::move(done));
}

void MemoryMessageStore::open_direct(const core::RoomId& room, const core::UserId& user,
                                     const core::UserId& peer,
                                     MessageCallback<MembershipChange> done) {
    create_listed(room, core::ports::RoomKind::DirectChat, user, {user, peer}, MemberRole::Member,
                  std::move(done));
}

void MemoryMessageStore::create_group(const core::RoomId& room, const core::UserId& creator,
                                      std::vector<core::UserId> members,
                                      MessageCallback<MembershipChange> done) {
    members.insert(members.begin(), creator);
    create_listed(room, core::ports::RoomKind::GroupChat, creator, std::move(members),
                  MemberRole::Admin, std::move(done));
}

void MemoryMessageStore::add_members(const core::RoomId& room, const core::UserId& actor,
                                     std::vector<core::UserId> users,
                                     MessageCallback<MembershipChange> done) {
    const auto recorded = kinds_.find(room);
    Members* listed = members_.contains(room) ? &members_[room] : nullptr;
    std::optional<MemberRole> role;
    if (listed != nullptr) {
        if (const auto at = listed->find(actor); at != listed->end()) {
            role = at->second;
        }
    }
    MembershipChange change = refusal(MembershipOutcome::Done);
    if (recorded == kinds_.end() || !role) {
        change = refusal(MembershipOutcome::NotMember);
    } else if (recorded->second != core::ports::RoomKind::GroupChat) {
        change = refusal(MembershipOutcome::NotGroup);
    } else if (*role != MemberRole::Admin) {
        change = refusal(MembershipOutcome::NotAdmin);
    } else {
        std::ranges::sort(users, ByteOrder{});
        const auto [first, last] = std::ranges::unique(users);
        users.erase(first, last);
        std::erase_if(users, [&](const core::UserId& u) { return listed->contains(u); });
        if (listed->size() + users.size() > core::ports::kMaxGroupMembers) {
            change = refusal(MembershipOutcome::Full);
        } else {
            for (const core::UserId& user : users) {
                listed->emplace(user, MemberRole::Member);
            }
            change.changed = std::move(users);
        }
    }
    answer_change(std::move(change), true, room, std::move(done));
}

void MemoryMessageStore::expel(const core::RoomId& room, const core::UserId& actor,
                               const core::UserId& user, MessageCallback<MembershipChange> done) {
    if (actor == user) {
        leave_room(room, user, std::move(done));
        return;
    }
    const auto recorded = kinds_.find(room);
    const auto listed = members_.find(room);
    std::optional<MemberRole> actor_at;
    if (listed != members_.end()) {
        if (const auto at = listed->second.find(actor); at != listed->second.end()) {
            actor_at = at->second;
        }
    }
    MembershipChange change = refusal(MembershipOutcome::Done);
    if (recorded == kinds_.end() || !actor_at) {
        change = refusal(MembershipOutcome::NotMember);
    } else if (recorded->second != core::ports::RoomKind::GroupChat) {
        change = refusal(MembershipOutcome::NotGroup);
    } else if (*actor_at != MemberRole::Admin) {
        change = refusal(MembershipOutcome::NotAdmin);
    } else if (listed->second.erase(user) != 0) {
        change.changed.push_back(user);
    }
    answer_change(std::move(change), false, room, std::move(done));
}

void MemoryMessageStore::leave_room(const core::RoomId& room, const core::UserId& user,
                                    MessageCallback<MembershipChange> done) {
    const auto recorded = kinds_.find(room);
    const auto listed = members_.find(room);
    MembershipChange change = refusal(MembershipOutcome::Done);
    if (recorded == kinds_.end() || listed == members_.end() || !listed->second.contains(user)) {
        change = refusal(MembershipOutcome::NotMember);
    } else if (recorded->second != core::ports::RoomKind::GroupChat) {
        change = refusal(MembershipOutcome::NotGroup);
    } else {
        Members& members = listed->second;
        const bool was_admin = members.at(user) == MemberRole::Admin;
        members.erase(user);
        change.changed.push_back(user);
        const bool admin_left = std::ranges::any_of(
            members, [](const auto& m) { return m.second == MemberRole::Admin; });
        if (was_admin && !admin_left && !members.empty()) {
            members.begin()->second = MemberRole::Admin;
            change.promoted = members.begin()->first;
        }
    }
    answer_change(std::move(change), false, room, std::move(done));
}

void MemoryMessageStore::rooms_of(const core::UserId& user, std::optional<core::RoomId> after,
                                  std::size_t limit,
                                  MessageCallback<std::vector<core::ports::RoomEntry>> done) {
    // Room ids in the durable store's order: uuid comparison is bytewise, which the canonical
    // lowercase text follows.
    std::map<std::string, core::ports::RoomEntry> found;
    for (const auto& [room, members] : members_) {
        const auto at = members.find(user);
        if (at == members.end()) {
            continue;
        }
        const std::string key = room.to_string();
        if (after && key <= after->to_string()) {
            continue;
        }
        core::ports::RoomEntry entry{.room = room,
                                     .kind = core::ports::RoomKind::GroupChat,
                                     .role = at->second,
                                     .peer = std::nullopt};
        if (const auto kind = kinds_.find(room); kind != kinds_.end()) {
            entry.kind = kind->second;
        }
        if (entry.kind == core::ports::RoomKind::DirectChat) {
            for (const auto& [other, role] : members) {
                if (other != user) {
                    entry.peer = other;
                    break;
                }
            }
        }
        found.emplace(key, entry);
    }
    std::vector<core::ports::RoomEntry> out;
    const std::size_t rows = std::min(limit, core::ports::kMaxListPage);
    for (const auto& [key, entry] : found) {
        if (out.size() == rows) {
            break;
        }
        out.push_back(entry);
    }
    defer([done = std::move(done), out = std::move(out)]() mutable noexcept {
        done(std::move(out));
    });
}

void MemoryMessageStore::roster(const core::RoomId& room, const core::UserId& asker,
                                std::optional<core::UserId> after, std::size_t limit,
                                MessageCallback<core::ports::Roster> done) {
    core::ports::Roster out;
    if (const auto it = members_.find(room); it != members_.end() && it->second.contains(asker)) {
        out.asker_listed = true;
        const std::size_t rows = std::min(limit, core::ports::kMaxListPage);
        const Members& members = it->second;
        for (auto from = after ? members.upper_bound(*after) : members.begin();
             from != members.end() && out.members.size() < rows; ++from) {
            out.members.push_back(
                core::ports::MemberEntry{.user = from->first, .role = from->second});
        }
    }
    defer([done = std::move(done), out = std::move(out)]() mutable noexcept {
        done(std::move(out));
    });
}

} // namespace infra::messages
