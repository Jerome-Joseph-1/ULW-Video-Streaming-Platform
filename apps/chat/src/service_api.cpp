// Chat's service API (ADR-0096): the operator's backend lists and unlists members on its users'
// behalf, through the same store changes as the users' own commands (membership.cpp).

#include "service_api.hpp"

#include "core/util/json.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "infra/auth/token_extractor.hpp"
#include "rt/message_key.hpp"

#include "envelope.hpp"
#include "named_rooms.hpp"
#include "presence_room.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <initializer_list>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace chat {

namespace {

using core::ports::MembershipChange;
using core::ports::MembershipOutcome;
using core::ports::MessageResult;
using http::Status;

// A refused request's answer has gone out and the write side is shut; the peer gets this long to
// read it and close before the socket goes.
constexpr core::Millis kLinger{1'000};
constexpr std::size_t kResponseHead = 512;
// Nothing the API reads nests deeper than an array in an object.
constexpr core::json::Limits kJsonLimits{.max_depth = 4, .max_bytes = std::size_t{64} * 1024};

enum class Op : std::uint8_t {
    None,
    OpenDirect,
    CreateGroup,
    AddMembers,
    RemoveMember,
    Rooms,
    Members,
};

Op op_of(std::string_view target) noexcept {
    if (!target.starts_with(kServicePrefix)) {
        return Op::None;
    }
    const std::string_view name = target.substr(kServicePrefix.size());
    constexpr std::array<std::pair<std::string_view, Op>, 6> kOps{{
        {"open_direct", Op::OpenDirect},
        {"create_group", Op::CreateGroup},
        {"add_members", Op::AddMembers},
        {"remove_member", Op::RemoveMember},
        {"rooms", Op::Rooms},
        {"members", Op::Members},
    }};
    const auto* found = std::ranges::find(kOps, name, &std::pair<std::string_view, Op>::first);
    return found == kOps.end() ? Op::None : found->second;
}

// A refusal: the status, and the reason in the body.
struct Refusal {
    Status status;
    std::string_view reason;
};

constexpr Refusal kMalformed{.status = Status::BadRequest, .reason = "malformed"};
constexpr Refusal kBadRoom{.status = Status::BadRequest, .reason = "bad_room"};
constexpr Refusal kBadUser{.status = Status::BadRequest, .reason = "bad_user"};

template <class T> using Parsed = std::expected<T, Refusal>;

// Only the fields a request defines: a misspelt optional one is refused, not ignored.
bool only(const core::json::Value& body, std::initializer_list<std::string_view> allowed) {
    const auto* members = body.as_object();
    return members != nullptr && std::ranges::all_of(*members, [&](const auto& m) {
               return std::ranges::find(allowed, m.first) != allowed.end();
           });
}

Parsed<core::RoomId> room_of(const core::json::Value& body, std::string_view field) {
    const core::json::Value* v = body.find(field);
    const auto text = v == nullptr ? std::nullopt : v->as_string();
    if (!text) {
        return std::unexpected(kMalformed);
    }
    const auto room = core::RoomId::parse(*text);
    if (!room || is_presence_room(*room)) {
        return std::unexpected(kBadRoom);
    }
    return *room;
}

Parsed<core::UserId> user_of(const core::json::Value& body, std::string_view field) {
    const core::json::Value* v = body.find(field);
    const auto text = v == nullptr ? std::nullopt : v->as_string();
    if (!text) {
        return std::unexpected(kMalformed);
    }
    const auto user = core::UserId::parse(*text);
    if (!user) {
        return std::unexpected(kBadUser);
    }
    return *user;
}

// `min` to `max` user ids; absent counts as none.
Parsed<std::vector<core::UserId>> users_of(const core::json::Value& body, std::size_t min,
                                           std::size_t max) {
    const core::json::Value* list = body.find("users");
    if (list == nullptr) {
        if (min == 0) {
            return std::vector<core::UserId>{};
        }
        return std::unexpected(kMalformed);
    }
    const auto* items = list->as_array();
    if (items == nullptr || items->size() < min || items->size() > max) {
        return std::unexpected(kMalformed);
    }
    std::vector<core::UserId> users;
    users.reserve(items->size());
    for (const core::json::Value& item : *items) {
        const auto text = item.as_string();
        if (!text) {
            return std::unexpected(kMalformed);
        }
        const auto user = core::UserId::parse(*text);
        if (!user) {
            return std::unexpected(kBadUser);
        }
        users.push_back(*user);
    }
    return users;
}

// "limit", 1 to kMaxListLimit, kDefaultListLimit when absent.
Parsed<std::size_t> limit_of(const core::json::Value& body) {
    const core::json::Value* limit = body.find("limit");
    if (limit == nullptr) {
        return kDefaultListLimit;
    }
    const auto n = limit->as_u64();
    if (!n || *n == 0 || *n > kMaxListLimit) {
        return std::unexpected(kMalformed);
    }
    return static_cast<std::size_t>(*n);
}

// Each once, in byte order, without `creator`: what the store takes as a group's members.
std::vector<core::UserId> others(std::vector<core::UserId> users, const core::UserId& creator) {
    std::ranges::sort(users, {}, &core::UserId::view);
    const auto [first, last] = std::ranges::unique(users);
    users.erase(first, last);
    std::erase(users, creator);
    return users;
}

void append_users(std::string& out, std::string_view field, std::span<const core::UserId> users) {
    out += ",\"";
    out += field;
    out += "\":[";
    bool first = true;
    for (const core::UserId& user : users) {
        if (!first) {
            out += ',';
        }
        first = false;
        core::json::append_string(out, user.view());
    }
    out += ']';
}

} // namespace

class ServiceApi::Connection final : public net::IStreamHandler,
                                     public http::IRequestSink,
                                     public net::ITimerHandler,
                                     public core::ports::IKeyWaiter {
public:
    Connection(ServiceApi& api, std::uint64_t id) noexcept : api_(api), id_(id), parser_(*this) {}
    ~Connection() override {
        cancel_timer();
        if (waiting_keys_) {
            api_.verifier_.cancel_wait(*this);
        }
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    [[nodiscard]] bool attach(os::UniqueFd fd) noexcept {
        auto conn = api_.reactor_.attach(std::move(fd), *this);
        if (!conn) {
            closed_ = true;
            return false;
        }
        conn_ = *conn;
        attached_ = true;
        api_.reactor_.start_receiving(conn_);
        arm(api_.limits_.idle_timeout);
        return true;
    }

    [[nodiscard]] bool released() const noexcept {
        return closed_ && (!attached_ || api_.reactor_.is_quiescent(conn_));
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        cancel_timer();
        if (waiting_keys_) {
            waiting_keys_ = false;
            api_.verifier_.cancel_wait(*this);
        }
        api_.reactor_.begin_close(conn_);
    }

    void on_data(net::BorrowedBytes bytes) noexcept override {
        if (closed_ || closing_) {
            return;
        }
        if (busy_) {
            // Behind a request still being answered: kept by the parser, read once it is.
            if (const auto kept = parser_.feed(bytes); !kept) {
                close();
            }
            return;
        }
        const core::MonoTime now = api_.reactor_.now();
        if (!deadline_) {
            deadline_ = now + api_.limits_.request_timeout;
        }
        const auto left = std::chrono::duration_cast<core::Millis>(*deadline_ - now);
        arm(std::max(core::Millis{0}, std::min(api_.limits_.idle_timeout, left)));
        handle(parser_.feed(bytes));
    }

    void on_writable() noexcept override {}
    void on_peer_eof() noexcept override { close(); }
    void on_error(int /*err*/) noexcept override { close(); }

    void on_timeout() noexcept override {
        timer_.reset();
        close();
    }

    void on_keys_refreshed() noexcept override {
        if (!waiting_keys_ || closed_) {
            return;
        }
        waiting_keys_ = false;
        authenticate();
        carry_on();
    }

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        body_.clear();
        authorization_.reset();
        too_large_ = false;
        retry_after_.reset();
        keep_alive_ = head.keep_alive;
        ServiceApiCounters& c = api_.counters_;
        ++c.requests;
        op_ = op_of(head.target);
        if (op_ == Op::None) {
            ++c.bad_requests;
            return http::HeadVerdict::reject(Status::NotFound);
        }
        if (head.method != http::Method::Post) {
            ++c.bad_requests;
            return http::HeadVerdict::reject(Status::MethodNotAllowed);
        }
        if (head.content_length > api_.limits_.max_body) {
            ++c.bad_requests;
            return http::HeadVerdict::reject(Status::ContentTooLarge);
        }
        const auto taken = api_.bucket_.take(api_.reactor_.now());
        if (!taken) {
            ++c.limited;
            retry_after_ = taken.error();
            return http::HeadVerdict::reject(Status::TooManyRequests);
        }
        // Only the header: a cookie is a browser's, and the backend is not one.
        if (const auto value = http::find_header(head.headers, "authorization")) {
            try {
                authorization_ = std::string(*value);
            } catch (const std::bad_alloc&) {
                return http::HeadVerdict::reject(Status::ServiceUnavailable);
            }
        }
        return http::HeadVerdict::accept();
    }

    http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override {
        if (too_large_ || body_.size() + bytes.size() > api_.limits_.max_body) {
            too_large_ = true;
            return http::BodyVerdict::Continue;
        }
        try {
            std::ranges::transform(bytes, std::back_inserter(body_),
                                   [](std::byte b) { return static_cast<char>(b); });
        } catch (const std::bad_alloc&) {
            too_large_ = true;
        }
        return http::BodyVerdict::Continue;
    }

    void on_message_complete() noexcept override {}

    // What the store answered a change or a listing; ignored once the connection closed.
    void answered(Status status, std::string_view body) noexcept {
        if (closed_ || closing_ || !busy_) {
            return;
        }
        respond(status, body);
        carry_on();
    }

    void refused(const Refusal& refusal) noexcept {
        if (closed_ || closing_ || !busy_) {
            return;
        }
        refuse_with(refusal);
        carry_on();
    }

private:
    // Parses what came, answering each whole request; a request answered later (by the store,
    // or once the keys are in) stops it until carry_on().
    void handle(http::ParseResult result) noexcept {
        in_handle_ = true;
        for (;;) {
            if (!result) {
                // Refused at its head, or not HTTP: nothing behind it can be read as a request.
                const Status status = result.error().status;
                if (status == Status::TooManyRequests) {
                    respond_error(status, "rate_limited", true);
                } else if (status == Status::NotFound) {
                    respond_error(status, "not_found", true);
                } else if (status == Status::MethodNotAllowed) {
                    respond_error(status, "method_not_allowed", true);
                } else if (status == Status::ContentTooLarge) {
                    respond_error(status, "too_large", true);
                } else {
                    ++api_.counters_.bad_requests;
                    respond_error(status, "malformed", true);
                }
                break;
            }
            if (*result != http::ParseProgress::MessageComplete) {
                break;
            }
            busy_ = true;
            api_.reactor_.stop_receiving(conn_);
            process();
            if (busy_ || closing_ || closed_) {
                break;
            }
            result = next();
        }
        in_handle_ = false;
    }

    // After an answer that came later than the request: the next request, if one is waiting.
    void carry_on() noexcept {
        if (in_handle_ || busy_ || closing_ || closed_) {
            return;
        }
        handle(next());
    }

    http::ParseResult next() noexcept {
        deadline_.reset();
        arm(api_.limits_.idle_timeout);
        parser_.reset_for_next_request();
        api_.reactor_.start_receiving(conn_);
        return parser_.resume();
    }

    void process() noexcept {
        if (too_large_) {
            ++api_.counters_.bad_requests;
            respond_error(Status::ContentTooLarge, "too_large");
            return;
        }
        authenticate();
    }

    void authenticate() noexcept {
        if (!authorization_) {
            ++api_.counters_.unauthorized;
            respond_error(Status::Unauthorized, "unauthorized");
            return;
        }
        infra::auth::TokenExtractor extractor("");
        extractor.on_header("authorization", *authorization_);
        const auto token = extractor.token();
        if (!token) {
            ++api_.counters_.unauthorized;
            respond_error(Status::Unauthorized, "unauthorized");
            return;
        }
        std::optional<core::ports::VerifyResult> result;
        try {
            result = api_.verifier_.verify(*token, api_.clock_.wall_now(), *this);
        } catch (const std::bad_alloc&) {
            ++api_.counters_.unavailable;
            respond_error(Status::ServiceUnavailable, "unavailable");
            return;
        }
        if (!result) {
            // Answered once the key set is refreshed (on_keys_refreshed).
            waiting_keys_ = true;
            return;
        }
        if (!*result) {
            if (result->error() == core::ports::AuthError::KeysUnavailable) {
                ++api_.counters_.unavailable;
                respond_error(Status::ServiceUnavailable, "unavailable");
                return;
            }
            ++api_.counters_.unauthorized;
            respond_error(Status::Unauthorized, "unauthorized");
            return;
        }
        if (!(*result)->is_service) {
            // A user's token, however valid: users manage lists over the socket, if at all.
            ++api_.counters_.forbidden;
            respond_error(Status::Forbidden, "forbidden");
            return;
        }
        try {
            dispatch();
        } catch (const std::bad_alloc&) {
            ++api_.counters_.unavailable;
            respond_error(Status::ServiceUnavailable, "unavailable");
        }
    }

    void dispatch() {
        const auto json = core::json::parse(body_, kJsonLimits);
        if (!json || json->as_object() == nullptr) {
            ++api_.counters_.bad_requests;
            respond_error(Status::BadRequest, json ? "malformed" : "not_json");
            return;
        }
        Parsed<void> started;
        switch (op_) {
        case Op::OpenDirect:
            started = open_direct(*json);
            break;
        case Op::CreateGroup:
            started = create_group(*json);
            break;
        case Op::AddMembers:
            started = add_members(*json);
            break;
        case Op::RemoveMember:
            started = remove_member(*json);
            break;
        case Op::Rooms:
            started = rooms(*json);
            break;
        case Op::Members:
            started = members(*json);
            break;
        case Op::None:
            started = std::unexpected(Refusal{.status = Status::NotFound, .reason = "not_found"});
            break;
        }
        if (!started) {
            ++api_.counters_.bad_requests;
            respond_error(started.error().status, started.error().reason);
        }
    }

    // Hands the store's answer to the connection that asked, if it is still there.
    template <class T, class Answer> [[nodiscard]] auto reply(Answer answer) {
        return [api = &api_, id = id_,
                answer = std::move(answer)](MessageResult<T> result) mutable noexcept {
            Connection* c = api->find(id);
            if (c == nullptr) {
                return;
            }
            if (!result) {
                ++api->counters_.unavailable;
                c->refused(Refusal{.status = Status::ServiceUnavailable, .reason = "unavailable"});
                return;
            }
            try {
                answer(*c, *api, std::move(*result));
            } catch (const std::bad_alloc&) {
                ++api->counters_.unavailable;
                c->refused(Refusal{.status = Status::ServiceUnavailable, .reason = "unavailable"});
            }
        };
    }

    // A refused change: what the users' commands answer, as a status the backend can act on.
    static Refusal refusal_of(MembershipOutcome outcome) noexcept {
        switch (outcome) {
        case MembershipOutcome::NotMember:
            return {.status = Status::Conflict, .reason = "not_member"};
        case MembershipOutcome::NotAdmin:
            return {.status = Status::Conflict, .reason = "not_admin"};
        case MembershipOutcome::NotGroup:
            return {.status = Status::Conflict, .reason = "not_group"};
        case MembershipOutcome::Full:
            return {.status = Status::Conflict, .reason = "too_many_members"};
        case MembershipOutcome::RoomLimit:
            return {.status = Status::Conflict, .reason = "room_limit"};
        case MembershipOutcome::Gone:
            return {.status = Status::Conflict, .reason = "gone"};
        case MembershipOutcome::WrongKind:
        case MembershipOutcome::Done:
            break;
        }
        // A named room is recorded only as its kind (migration 0015): the store answered what
        // nothing here can have written.
        return {.status = Status::ServiceUnavailable, .reason = "unavailable"};
    }

    // Done is answered by `write`; anything else is refused as refusal_of says, or as
    // `on_not_member` says for NotMember when given.
    template <class Write>
    void change_answer(ServiceApi& api, const MembershipChange& change, Write write,
                       std::optional<Refusal> on_not_member = std::nullopt) {
        if (change.outcome != MembershipOutcome::Done) {
            ++api.counters_.refused;
            refused(change.outcome == MembershipOutcome::NotMember && on_not_member
                        ? *on_not_member
                        : refusal_of(change.outcome));
            return;
        }
        ++api.counters_.changes;
        std::string out;
        write(out);
        answered(Status::Ok, out);
    }

    Parsed<void> open_direct(const core::json::Value& body) {
        if (!only(body, {"users"})) {
            return std::unexpected(kMalformed);
        }
        auto users = users_of(body, 2, 2);
        if (!users) {
            return std::unexpected(users.error());
        }
        const core::UserId first = (*users)[0];
        const core::UserId second = (*users)[1];
        if (first == second) {
            return std::unexpected(Refusal{.status = Status::BadRequest, .reason = "self"});
        }
        const core::RoomId room = direct_room(first, second);
        api_.store_.open_direct(
            room, first, second,
            reply<MembershipChange>([room, users = std::move(*users)](
                                        Connection& c, ServiceApi& api, MembershipChange change) {
                c.change_answer(api, change, [&](std::string& out) {
                    out += R"({"type":"direct","room":")";
                    out += room.to_string();
                    out += '"';
                    append_users(out, "users", users);
                    append_users(out, "added", change.changed);
                    out += '}';
                });
            }));
        return {};
    }

    Parsed<void> create_group(const core::json::Value& body) {
        if (!only(body, {"creator", "id", "users"})) {
            return std::unexpected(kMalformed);
        }
        auto creator = user_of(body, "creator");
        if (!creator) {
            return std::unexpected(creator.error());
        }
        const core::json::Value* id_value = body.find("id");
        const auto id_text = id_value == nullptr ? std::nullopt : id_value->as_string();
        if (!id_text) {
            return std::unexpected(kMalformed);
        }
        const auto request = rt::MessageKey::parse(*id_text);
        if (!request) {
            return std::unexpected(Refusal{.status = Status::BadRequest, .reason = "bad_id"});
        }
        auto users = users_of(body, 0, core::ports::kMaxMembersPerChange);
        if (!users) {
            return std::unexpected(users.error());
        }
        const core::RoomId room = group_room(*creator, *request);
        api_.store_.create_group(
            room, *creator, others(std::move(*users), *creator),
            reply<MembershipChange>([room, request = *request](Connection& c, ServiceApi& api,
                                                               MembershipChange change) {
                c.change_answer(api, change, [&](std::string& out) {
                    out += R"({"type":"group","room":")";
                    out += room.to_string();
                    out += R"(","id":)";
                    core::json::append_string(out, request.view());
                    append_users(out, "added", change.changed);
                    out += '}';
                });
            }));
        return {};
    }

    Parsed<void> add_members(const core::json::Value& body) {
        if (!only(body, {"room", "users"})) {
            return std::unexpected(kMalformed);
        }
        auto room = room_of(body, "room");
        if (!room) {
            return std::unexpected(room.error());
        }
        auto users = users_of(body, 1, core::ports::kMaxMembersPerChange);
        if (!users) {
            return std::unexpected(users.error());
        }
        std::ranges::sort(*users, {}, &core::UserId::view);
        const auto [first, last] = std::ranges::unique(*users);
        users->erase(first, last);
        api_.store_.add_members(
            *room, core::ports::Actor::service(), std::move(*users),
            reply<MembershipChange>([room = *room](Connection& c, ServiceApi& api,
                                                   MembershipChange change) {
                c.change_answer(
                    api, change, [&](std::string& out) { write_added(out, room, change.changed); },
                    Refusal{.status = Status::NotFound, .reason = "no_room"});
            }));
        return {};
    }

    Parsed<void> remove_member(const core::json::Value& body) {
        if (!only(body, {"room", "user"})) {
            return std::unexpected(kMalformed);
        }
        auto room = room_of(body, "room");
        if (!room) {
            return std::unexpected(room.error());
        }
        auto user = user_of(body, "user");
        if (!user) {
            return std::unexpected(user.error());
        }
        // A removal by the operator is the user leaving: the group keeps an admin while it has
        // members, and everyone concerned is told, as for any leave.
        api_.store_.leave_room(
            *room, *user,
            reply<MembershipChange>([room = *room, user = *user](Connection& c, ServiceApi& api,
                                                                 MembershipChange change) {
                c.change_answer(
                    api, change,
                    [&](std::string& out) {
                        write_removed(out, room, user);
                        if (change.promoted) {
                            out.pop_back();
                            out += R"(,"promoted":)";
                            core::json::append_string(out, change.promoted->view());
                            out += '}';
                        }
                    },
                    Refusal{.status = Status::NotFound, .reason = "not_member"});
            }));
        return {};
    }

    Parsed<void> rooms(const core::json::Value& body) {
        if (!only(body, {"user", "after", "limit"})) {
            return std::unexpected(kMalformed);
        }
        auto user = user_of(body, "user");
        if (!user) {
            return std::unexpected(user.error());
        }
        std::optional<core::RoomId> after;
        if (body.find("after") != nullptr) {
            auto room = room_of(body, "after");
            if (!room) {
                return std::unexpected(room.error());
            }
            after = *room;
        }
        const auto limit = limit_of(body);
        if (!limit) {
            return std::unexpected(limit.error());
        }
        // One more than the page, to say whether there is more.
        api_.store_.rooms_of(*user, after, *limit + 1,
                             reply<std::vector<core::ports::RoomEntry>>(
                                 [limit = *limit](Connection& c, ServiceApi& api,
                                                  std::vector<core::ports::RoomEntry> rooms) {
                                     ++api.counters_.reads;
                                     std::string out;
                                     write_rooms(
                                         out, std::span(rooms).first(std::min(limit, rooms.size())),
                                         rooms.size() > limit);
                                     c.answered(Status::Ok, out);
                                 }));
        return {};
    }

    Parsed<void> members(const core::json::Value& body) {
        if (!only(body, {"room", "after", "limit"})) {
            return std::unexpected(kMalformed);
        }
        auto room = room_of(body, "room");
        if (!room) {
            return std::unexpected(room.error());
        }
        std::optional<core::UserId> after;
        if (body.find("after") != nullptr) {
            auto user = user_of(body, "after");
            if (!user) {
                return std::unexpected(user.error());
            }
            after = *user;
        }
        const auto limit = limit_of(body);
        if (!limit) {
            return std::unexpected(limit.error());
        }
        api_.store_.roster(
            *room, core::ports::Actor::service(), after, *limit + 1,
            reply<core::ports::Roster>([room = *room,
                                        limit = *limit](Connection& c, ServiceApi& api,
                                                        const core::ports::Roster& roster) {
                ++api.counters_.reads;
                const auto& members = roster.members;
                std::string out;
                write_members(out, room, std::span(members).first(std::min(limit, members.size())),
                              members.size() > limit);
                c.answered(Status::Ok, out);
            }));
        return {};
    }

    void refuse_with(const Refusal& refusal) noexcept {
        respond_error(refusal.status, refusal.reason);
    }

    // `close`: the connection ends with this answer.
    void respond_error(Status status, std::string_view reason, bool close = false) noexcept {
        std::string body;
        try {
            body = R"({"type":"error","reason":)";
            core::json::append_string(body, reason);
            if (retry_after_) {
                body += R"(,"retry_after_ms":)";
                body += std::to_string(retry_after_->count());
            }
            body += '}';
        } catch (const std::bad_alloc&) {
            body.clear();
        }
        respond(status, body, close);
    }

    void respond(Status status, std::string_view body, bool close = false) noexcept {
        busy_ = false;
        const bool keep = keep_alive_ && !close;
        std::optional<std::chrono::seconds> retry;
        if (retry_after_) {
            retry = std::max(std::chrono::seconds{1},
                             std::chrono::ceil<std::chrono::seconds>(*retry_after_));
        } else if (status == Status::ServiceUnavailable) {
            retry = std::chrono::seconds{1};
        }
        std::array<char, kResponseHead> buf{};
        const auto n = http::write_response_head(
            {.status = status,
             .content_length = body.size(),
             .connection = keep ? http::Connection::KeepAlive : http::Connection::Close,
             .content_type = "application/json",
             .retry_after = retry,
             .www_authenticate = status == Status::Unauthorized ? "Bearer" : "",
             .allow = status == Status::MethodNotAllowed ? http::MethodSet{http::Method::Post}
                                                         : http::MethodSet{}},
            buf);
        if (!n) {
            const std::string_view fixed =
                http::fixed_response(Status::InternalServerError, http::Connection::Close);
            api_.reactor_.send(conn_, std::as_bytes(std::span(fixed)));
            linger();
            return;
        }
        api_.reactor_.send(conn_, std::as_bytes(std::span(buf.data(), *n)));
        api_.reactor_.send(conn_, std::as_bytes(std::span(body)));
        if (!keep) {
            linger();
        }
    }

    void linger() noexcept {
        if (closing_ || closed_) {
            return;
        }
        closing_ = true;
        busy_ = false;
        api_.reactor_.shutdown_write(conn_);
        arm(kLinger);
    }

    void arm(core::Millis delay) noexcept {
        cancel_timer();
        timer_ = api_.reactor_.arm_timer(delay, *this);
    }

    void cancel_timer() noexcept {
        if (timer_) {
            api_.reactor_.cancel_timer(*timer_);
            timer_.reset();
        }
    }

    ServiceApi& api_;
    std::uint64_t id_;
    http::RequestParser parser_;
    net::ConnId conn_;
    std::optional<net::TimerId> timer_;
    Op op_ = Op::None;
    std::string body_;
    std::optional<std::string> authorization_;
    std::optional<core::Millis> retry_after_;
    // When the request under way must be in.
    std::optional<core::MonoTime> deadline_;
    bool keep_alive_ = true;
    bool too_large_ = false;
    // A whole request is being answered: nothing more is parsed until it is.
    bool busy_ = false;
    bool in_handle_ = false;
    bool waiting_keys_ = false;
    // The answer is out and the write side shut: closed once the peer has read it.
    bool closing_ = false;
    bool attached_ = false;
    bool closed_ = false;
};

ServiceApi::ServiceApi(net::IReactor& reactor, const core::ports::IClock& clock,
                       core::ports::IJwtVerifier& verifier, core::ports::IMessageStore& store,
                       ServiceApiLimits limits)
    : reactor_(reactor), clock_(clock), verifier_(verifier), store_(store), limits_(limits),
      bucket_(limits.burst, limits.per_second, reactor.now()) {}

ServiceApi::~ServiceApi() {
    for (const auto& c : connections_) {
        c->close();
    }
}

ServiceApi::Connection* ServiceApi::find(std::uint64_t id) noexcept {
    const auto it = std::ranges::find(connections_, id, &Connection::id);
    return it == connections_.end() ? nullptr : it->get();
}

void ServiceApi::on_accept(os::UniqueFd conn) noexcept {
    reap();
    if (stopped_ || connections_.size() >= limits_.max_connections) {
        ++counters_.refused_connections;
        return;
    }
    try {
        auto made = std::make_unique<Connection>(*this, next_id_++);
        if (!made->attach(std::move(conn))) {
            ++counters_.refused_connections;
            return;
        }
        ++counters_.connections;
        connections_.push_back(std::move(made));
    } catch (const std::bad_alloc&) {
        ++counters_.refused_connections;
    }
}

void ServiceApi::reap() noexcept {
    std::erase_if(connections_, [](const std::unique_ptr<Connection>& c) { return c->released(); });
}

void ServiceApi::stop() noexcept {
    stopped_ = true;
    for (const auto& c : connections_) {
        c->close();
    }
}

} // namespace chat
