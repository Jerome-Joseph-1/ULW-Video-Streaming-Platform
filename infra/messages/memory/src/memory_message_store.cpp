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
    members_[room].insert(user);
    defer([done = std::move(done)]() mutable noexcept { done({}); });
}

void MemoryMessageStore::remove_member(const core::RoomId& room, const core::UserId& user,
                                       MessageCallback<void> done) {
    if (const auto it = members_.find(room); it != members_.end()) {
        it->second.erase(user);
    }
    defer([done = std::move(done)]() mutable noexcept { done({}); });
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
            out.push_back(*from);
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
