// The chat service's member-list commands (ADR-0096): a direct chat opened, a group chat created,
// members added and removed by a group's admins, a member leaving, and the listings of a user's
// rooms and a room's members. The store decides each change from the list as it stands, under
// the room's lock; this side bounds how often a user may ask, answers the client that asked, and
// tells clients on this node of every change the store announces, whichever node made it.

#include "chat_service.hpp"
#include "named_rooms.hpp"

#include <algorithm>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace chat {

namespace {

using core::ports::MembershipChange;
using core::ports::MembershipOutcome;
using core::ports::MessageResult;

// The users a change names, as the store takes them: each once, and never the one asking, who is
// listed by the change itself (a create) or must be already (an add).
[[nodiscard]] std::vector<core::UserId> others(std::vector<core::UserId> users,
                                               const core::UserId& asker) {
    std::ranges::sort(users, {}, &core::UserId::view);
    const auto [first, last] = std::ranges::unique(users);
    users.erase(first, last);
    std::erase(users, asker);
    return users;
}

} // namespace

std::expected<void, core::Millis> ChatService::admit_membership(const core::UserId& user) {
    const core::MonoTime now = clock_.now();
    PacedBucket& bucket =
        memberships_.try_emplace(user, limits_.membership_burst, limits_.membership_interval, now)
            .first->second;
    if (!bucket.available(now)) {
        return std::unexpected(bucket.wait(now));
    }
    bucket.take(now);
    return {};
}

void ChatService::refuse(IClient& client, std::string_view reason,
                         const ErrorContext& context) noexcept {
    try {
        std::string out;
        write_error_with(out, reason, context);
        client.push(out);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        client.allocation_failed();
    }
}

void ChatService::changed(ClientId id, const ErrorContext& context,
                          MessageResult<MembershipChange> result, ChangeAnswer answered) noexcept {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    if (!result) {
        ++counters_.membership_unavailable;
        refuse(*c->client, "unavailable", context);
        return;
    }
    switch (result->outcome) {
    case MembershipOutcome::Done:
        break;
    case MembershipOutcome::NotMember:
        ++counters_.membership_not_member;
        refuse(*c->client, "not_member", context);
        return;
    case MembershipOutcome::NotAdmin:
        ++counters_.membership_not_admin;
        refuse(*c->client, "not_admin", context);
        return;
    case MembershipOutcome::NotGroup:
        ++counters_.membership_not_group;
        refuse(*c->client, "not_group", context);
        return;
    case MembershipOutcome::Full:
        ++counters_.membership_full;
        refuse(*c->client, "too_many_members", context);
        return;
    case MembershipOutcome::RoomLimit:
        ++counters_.membership_room_limit;
        refuse(*c->client, "room_limit", context);
        return;
    case MembershipOutcome::Gone:
        ++counters_.membership_gone;
        refuse(*c->client, "gone", context);
        return;
    case MembershipOutcome::WrongKind:
        // A named room is recorded only as the kind its id names (migration 0015), so the store
        // has answered what nothing here can have written.
        ++counters_.membership_unavailable;
        refuse(*c->client, "unavailable", context);
        return;
    }
    try {
        std::string out;
        answered(out, *result);
        c->client->push(out);
    } catch (const std::bad_alloc&) {
        ++counters_.allocation_failures;
        c->client->allocation_failed();
    }
}

void ChatService::open_direct(ClientId id, const OpenDirect& open) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const ErrorContext context{
        .room = std::nullopt, .id = std::nullopt, .user = open.user, .retry_after = std::nullopt};
    // No user table to look the peer up in (ADR-0018): any user id may be named, and a room with
    // someone who never signs in is two rows nobody reads.
    if (open.user == c->user) {
        refuse(*c->client, "self", context);
        return;
    }
    if (const auto admitted = admit_membership(c->user); !admitted) {
        ++counters_.membership_rate_limited;
        refuse(*c->client, "rate_limited",
               ErrorContext{.room = std::nullopt,
                            .id = std::nullopt,
                            .user = open.user,
                            .retry_after = admitted.error()});
        return;
    }
    const core::RoomId room = direct_room(c->user, open.user);
    messages_.open_direct(
        room, c->user, open.user,
        [this, id, room, peer = open.user,
         context](MessageResult<MembershipChange> result) noexcept {
            if (result && result->outcome == MembershipOutcome::Done && !result->changed.empty()) {
                ++counters_.directs_opened;
            }
            changed(id, context, std::move(result),
                    [room, peer](std::string& out, const MembershipChange& /*change*/) {
                        write_direct(out, room, peer);
                    });
        });
}

void ChatService::create_group(ClientId id, CreateGroup create) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const ErrorContext context{
        .room = std::nullopt, .id = create.id, .user = std::nullopt, .retry_after = std::nullopt};
    if (const auto admitted = admit_membership(c->user); !admitted) {
        ++counters_.membership_rate_limited;
        refuse(*c->client, "rate_limited",
               ErrorContext{.room = std::nullopt,
                            .id = create.id,
                            .user = std::nullopt,
                            .retry_after = admitted.error()});
        return;
    }
    const core::RoomId room = group_room(c->user, create.id);
    messages_.create_group(
        room, c->user, others(std::move(create.users), c->user),
        [this, id, room, request = create.id,
         context](MessageResult<MembershipChange> result) noexcept {
            if (result && result->outcome == MembershipOutcome::Done && !result->changed.empty()) {
                ++counters_.groups_created;
            }
            changed(id, context, std::move(result),
                    [room, request](std::string& out, const MembershipChange& /*change*/) {
                        write_group(out, room, request);
                    });
        });
}

void ChatService::add_members(ClientId id, AddMembers add) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const ErrorContext context{
        .room = add.room, .id = std::nullopt, .user = std::nullopt, .retry_after = std::nullopt};
    if (const auto admitted = admit_membership(c->user); !admitted) {
        ++counters_.membership_rate_limited;
        refuse(*c->client, "rate_limited",
               ErrorContext{.room = add.room,
                            .id = std::nullopt,
                            .user = std::nullopt,
                            .retry_after = admitted.error()});
        return;
    }
    // The asker is listed already or is refused: naming themselves adds nobody.
    messages_.add_members(
        add.room, c->user, others(std::move(add.users), c->user),
        [this, id, room = add.room, context](MessageResult<MembershipChange> result) noexcept {
            if (result && result->outcome == MembershipOutcome::Done) {
                counters_.members_added += result->changed.size();
            }
            changed(id, context, std::move(result),
                    [room](std::string& out, const MembershipChange& change) {
                        write_added(out, room, change.changed);
                    });
        });
}

void ChatService::remove_member(ClientId id, const RemoveMember& remove) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const ErrorContext context{
        .room = remove.room, .id = std::nullopt, .user = remove.user, .retry_after = std::nullopt};
    if (const auto admitted = admit_membership(c->user); !admitted) {
        ++counters_.membership_rate_limited;
        refuse(*c->client, "rate_limited",
               ErrorContext{.room = remove.room,
                            .id = std::nullopt,
                            .user = remove.user,
                            .retry_after = admitted.error()});
        return;
    }
    // Removing oneself is leaving, which the store does for it, promotion included.
    const bool self = remove.user == c->user;
    messages_.expel(remove.room, c->user, remove.user,
                    [this, id, room = remove.room, user = remove.user, self,
                     context](MessageResult<MembershipChange> result) noexcept {
                        if (result && result->outcome == MembershipOutcome::Done &&
                            !result->changed.empty()) {
                            ++(self ? counters_.members_left : counters_.members_removed);
                        }
                        changed(id, context, std::move(result),
                                [room, user](std::string& out, const MembershipChange& /*change*/) {
                                    write_removed(out, room, user);
                                });
                    });
}

void ChatService::leave_room(ClientId id, const LeaveRoom& leave) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const ErrorContext context{
        .room = leave.room, .id = std::nullopt, .user = std::nullopt, .retry_after = std::nullopt};
    if (const auto admitted = admit_membership(c->user); !admitted) {
        ++counters_.membership_rate_limited;
        refuse(*c->client, "rate_limited",
               ErrorContext{.room = leave.room,
                            .id = std::nullopt,
                            .user = std::nullopt,
                            .retry_after = admitted.error()});
        return;
    }
    messages_.leave_room(
        leave.room, c->user,
        [this, id, room = leave.room, context](MessageResult<MembershipChange> result) noexcept {
            if (result && result->outcome == MembershipOutcome::Done) {
                ++counters_.members_left;
            }
            changed(id, context, std::move(result),
                    [room](std::string& out, const MembershipChange& change) {
                        write_left(out, room, change.promoted);
                    });
        });
}

void ChatService::list_rooms(ClientId id, const ListRooms& list) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    // A read of the store, as a history page is: charged to the same allowance.
    if (!admit_join(c->user)) {
        refuse(*c->client, "busy", ErrorContext{});
        return;
    }
    // One more than the page, to say whether there is more.
    messages_.rooms_of(
        c->user, list.after, list.limit + 1,
        [this, id,
         limit = list.limit](MessageResult<std::vector<core::ports::RoomEntry>> rooms) noexcept {
            Client* client = find(id);
            if (client == nullptr) {
                return;
            }
            if (!rooms) {
                ++counters_.membership_unavailable;
                refuse(*client->client, "unavailable", ErrorContext{});
                return;
            }
            const bool more = rooms->size() > limit;
            try {
                std::string out;
                write_rooms(out, std::span(*rooms).first(std::min(limit, rooms->size())), more);
                client->client->push(out);
            } catch (const std::bad_alloc&) {
                ++counters_.allocation_failures;
                client->client->allocation_failed();
            }
        });
}

void ChatService::list_members(ClientId id, const ListMembers& list) {
    Client* c = find(id);
    if (c == nullptr) {
        return;
    }
    const ErrorContext context{
        .room = list.room, .id = std::nullopt, .user = std::nullopt, .retry_after = std::nullopt};
    if (!admit_join(c->user)) {
        refuse(*c->client, "busy", context);
        return;
    }
    messages_.roster(list.room, c->user, list.after, list.limit + 1,
                     [this, id, room = list.room, limit = list.limit,
                      context](MessageResult<core::ports::Roster> roster) noexcept {
                         Client* client = find(id);
                         if (client == nullptr) {
                             return;
                         }
                         if (!roster) {
                             ++counters_.membership_unavailable;
                             refuse(*client->client, "unavailable", context);
                             return;
                         }
                         if (!roster->asker_listed) {
                             ++counters_.membership_not_member;
                             refuse(*client->client, "not_member", context);
                             return;
                         }
                         const auto& members = roster->members;
                         const bool more = members.size() > limit;
                         try {
                             std::string out;
                             write_members(
                                 out, room,
                                 std::span(members).first(std::min(limit, members.size())), more);
                             client->client->push(out);
                         } catch (const std::bad_alloc&) {
                             ++counters_.allocation_failures;
                             client->client->allocation_failed();
                         }
                     });
}

void ChatService::on_member_added(const core::RoomId& room, const core::UserId& user) noexcept {
    if (core::ports::is_stream_chat(room)) {
        return;
    }
    tell_members(room, user, "added");
}

void ChatService::on_member_role(const core::RoomId& room, const core::UserId& user,
                                 core::ports::MemberRole role) noexcept {
    if (core::ports::is_stream_chat(room)) {
        return;
    }
    tell_members(room, user, role == core::ports::MemberRole::Admin ? "promoted" : "demoted");
}

void ChatService::tell_members(const core::RoomId& room, const core::UserId& user,
                               std::string_view change) noexcept {
    std::string out;
    try {
        write_member_change(out, room, user, change);
    } catch (const std::bad_alloc&) {
        // Nobody is told; the change itself stands, and a listing shows it.
        ++counters_.allocation_failures;
        return;
    }
    for (auto& [value, c] : clients_) {
        // A join still waiting for the member list is not in the room: the answer may yet be
        // no, and until it comes the client hears nothing of the room but its own user's changes.
        if (c.user != user && (std::ranges::find(c.rooms, room) == c.rooms.end() ||
                               std::ranges::find(c.admitting, room) != c.admitting.end())) {
            continue;
        }
        if (c.client->push(out)) {
            ++counters_.member_events;
        }
    }
}

} // namespace chat
