// Chat's service API (ADR-0096): the operator's backend lists and unlists members, authenticated
// by the identity provider's token with the service claim, through the store's own changes.
#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "infra/notify/memory_connectivity.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"

#include "call.hpp"
#include "live_chat.hpp"
#include "named_rooms.hpp"
#include "presence_room.hpp"
#include "service_api.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_verifier.hpp"
#include "support/no_membership_store.hpp"
#include "support/reactor_harness.hpp"

#include <sys/socket.h>

#include <array>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using chat::ServiceApi;
using chat::ServiceApiLimits;
using ulw::test::FakeClock;
using ulw::test::pump_until;

core::UserId user(std::string_view id) {
    return *core::UserId::parse(id);
}

// What the store told of every change, as the room's notifications would reach every node.
class Heard final : public core::ports::IMemberListener {
public:
    void on_member_removed(const core::RoomId& room, const core::UserId& who) noexcept override {
        changes.push_back(std::format("- {} {}", room.to_string(), who.view()));
    }
    void on_member_added(const core::RoomId& room, const core::UserId& who) noexcept override {
        changes.push_back(std::format("+ {} {}", room.to_string(), who.view()));
    }
    void on_member_role(const core::RoomId& room, const core::UserId& who,
                        core::ports::MemberRole role) noexcept override {
        changes.push_back(std::format("* {} {} {}", room.to_string(),
                                      role == core::ports::MemberRole::Admin ? "admin" : "member",
                                      who.view()));
    }
    void on_members_resync() noexcept override {}
    void on_user_disconnected(const core::UserId& who) noexcept override {
        changes.push_back(std::format("! {}", who.view()));
    }

    std::vector<std::string> take() { return std::exchange(changes, {}); }

    std::vector<std::string> changes;
};

// A store that cannot be reached: every call is answered Unavailable.
class DownStore final : public ulw::test::NoMembershipStore {
public:
    void history_before(
        const core::RoomId& /*room*/, std::optional<std::uint64_t> /*before*/,
        std::size_t /*limit*/,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void history_after(
        const core::RoomId& /*room*/, std::uint64_t /*after*/, std::size_t /*limit*/,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void last_seq(const core::RoomId& /*room*/,
                  core::ports::MessageCallback<std::uint64_t> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void last_loud_seq(const core::RoomId& /*room*/,
                       core::ports::MessageCallback<std::uint64_t> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void add_member(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                    core::ports::MessageCallback<void> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void remove_member(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                       core::ports::MessageCallback<void> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void members(const core::RoomId& /*room*/, std::optional<core::UserId> /*after*/,
                 std::size_t /*limit*/,
                 core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    using core::ports::IMessageStore::admits;
    void admits(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                core::ports::RoomKind /*asked*/, core::ports::Recording /*recording*/,
                core::ports::MessageCallback<core::ports::Admission> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void access(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                core::ports::MessageCallback<core::ports::RoomAccess> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void record_live(const core::RoomId& /*room*/,
                     core::ports::MessageCallback<void> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void watch_members(core::ports::IMemberListener* /*listener*/) noexcept override {}
};

// The room plane as the service API's call operations use it (ADR-0111): a join, one ask of the
// room's owner, a leave. The owner answers `answer`, at once or when told to.
class FakeCallRooms final : public chat::IRooms {
public:
    void join(const core::RoomId& room, rt::IMember& /*member*/, rt::JoinCallback done) override {
        ++joins;
        joined_room = room;
        done(join_result);
    }
    void leave(const core::RoomId& /*room*/, rt::IMember& /*member*/) noexcept override {
        ++leaves;
        held = nullptr;
    }
    void send(const core::RoomId& /*room*/, rt::IMember& /*from*/, const core::UserId& /*sender*/,
              const rt::MessageKey& /*key*/, std::vector<std::byte> /*body*/, bool /*quiet*/,
              std::optional<core::ports::Relation> /*rel*/, rt::SendCallback done) override {
        done(std::unexpected(rt::RouteError::Unavailable));
    }
    void ask_owner(const core::RoomId& /*room*/, rt::IMember& /*from*/,
                   std::span<const std::byte> request, rt::OwnerAnswer done) override {
        asked = chat::decode_request(request);
        if (hold) {
            held = std::move(done);
            return;
        }
        done(answer);
    }
    void notify(const core::RoomId& room, std::span<const std::byte> notice) noexcept override {
        notices.emplace_back(room, std::vector<std::byte>(notice.begin(), notice.end()));
    }
    std::vector<std::pair<core::RoomId, std::vector<std::byte>>> notices;
    void reply(chat::CallOutcome outcome) {
        answer = chat::encode_answer({.outcome = outcome,
                                      .ticket = std::nullopt,
                                      .call = std::nullopt,
                                      .caller = outcome == chat::CallOutcome::Done
                                                    ? std::optional(user("alice"))
                                                    : std::nullopt});
    }

    std::expected<std::uint64_t, rt::RouteError> join_result = 0;
    std::expected<std::vector<std::byte>, rt::RouteError> answer;
    bool hold = false;
    rt::OwnerAnswer held;
    std::optional<chat::CallAsk> asked;
    std::optional<core::RoomId> joined_room;
    int joins = 0;
    int leaves = 0;
};

// One HTTP response as it came: status, the head's lines, the body.
struct Answer {
    int status = 0;
    std::string head;
    std::string body;

    [[nodiscard]] std::string header(std::string_view name) const {
        const std::string key = "\r\n" + std::string(name) + ": ";
        const auto at = head.find(key);
        if (at == std::string::npos) {
            return {};
        }
        const auto from = at + key.size();
        return head.substr(from, head.find("\r\n", from) - from);
    }
    // A top-level string field of the JSON body; "" when absent.
    [[nodiscard]] std::string field(std::string_view key) const {
        const auto json = core::json::parse(body);
        const core::json::Value* v = json ? json->find(key) : nullptr;
        return v == nullptr ? "" : std::string(v->as_string().value_or(""));
    }
};

// Cuts a whole response off the front of `bytes`; the rest stays for the next.
std::optional<Answer> cut(std::string& bytes) {
    const auto end = bytes.find("\r\n\r\n");
    if (end == std::string::npos) {
        return std::nullopt;
    }
    Answer a;
    a.head = bytes.substr(0, end);
    a.status = core::parse_integer<int>(std::string_view(a.head).substr(9, 3)).value_or(0);
    const std::size_t length =
        core::parse_integer<std::size_t>(a.header("Content-Length")).value_or(0);
    if (bytes.size() < end + 4 + length) {
        return std::nullopt;
    }
    a.body = bytes.substr(end + 4, length);
    bytes.erase(0, end + 4 + length);
    return a;
}

std::string request(std::string_view path, std::string_view body,
                    std::string_view token = "service.backend", std::string_view method = "POST",
                    std::string_view extra = "") {
    std::string r = std::format("{} {} HTTP/1.1\r\nHost: chat-service\r\n", method, path);
    if (!token.empty()) {
        r += "Authorization: Bearer " + std::string(token) + "\r\n";
    }
    r += extra;
    r += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n";
    r += body;
    return r;
}

std::string path(std::string_view op) {
    return std::string(chat::kServicePrefix) + std::string(op);
}

// The defaults, but room for the many connections one test makes from 127.0.0.1, each closed by
// the client as soon as answered, perhaps before the API has seen it go.
ServiceApiLimits roomy() {
    ServiceApiLimits limits;
    limits.max_connections_per_address = limits.max_connections;
    return limits;
}

class ServiceApiTest : public ::testing::Test {
protected:
    void SetUp() override { start(roomy()); }

    void TearDown() override {
        clients_.clear();
        // The store first, as the API's contract asks.
        store_.reset();
        api_.reset();
        reactor_.reset();
    }

    void start(const ServiceApiLimits& limits, bool down = false, bool with_rooms = true) {
        clients_.clear();
        store_.reset();
        api_.reset();
        reactor_.reset();
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock_, 256);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
        if (down) {
            store_ = std::make_unique<DownStore>();
        } else {
            auto memory = std::make_unique<infra::messages::MemoryMessageStore>(*reactor_);
            memory->watch_members(&heard_);
            store_ = std::move(memory);
        }
        api_ = std::make_unique<ServiceApi>(*reactor_, clock_, verifier_, *store_, limits,
                                            with_rooms ? &rooms_ : nullptr);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(listener);
        port_ = *net::local_port(listener->get());
        ASSERT_TRUE(reactor_->listen(std::move(*listener), *api_));
    }

    int connect() {
        clients_.push_back(ulw::test::connect_loopback(port_));
        EXPECT_TRUE(clients_.back());
        return clients_.back().get();
    }

    // A connection the API takes to come from `peer`, which loopback cannot be.
    int connect_from(std::string_view peer) {
        auto [ours, theirs] = ulw::test::unix_pair();
        api_->accept_from(std::move(theirs), *net::IpAddress::parse(peer));
        clients_.push_back(std::move(ours));
        return clients_.back().get();
    }

    // Appends what is there; true when the peer has closed.
    static bool read_into(int fd, std::string& got) {
        std::array<char, 4096> buf{};
        for (;;) {
            const ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
            if (n == 0) {
                return true;
            }
            if (n < 0) {
                return false;
            }
            got.append(buf.data(), static_cast<std::size_t>(n));
        }
    }

    // Sends `bytes` on `fd` and returns the next `count` answers on it.
    std::vector<Answer> exchange(int fd, std::string_view bytes, std::size_t count = 1) {
        std::size_t sent = 0;
        std::vector<Answer> answers;
        std::string& got = pending_[fd];
        bool closed = false;
        const bool done = pump_until(*reactor_, [&] {
            if (sent < bytes.size()) {
                sent += ulw::test::write_some(fd, std::as_bytes(std::span(bytes.substr(sent))));
            }
            closed = read_into(fd, got) || closed;
            while (auto a = cut(got)) {
                answers.push_back(std::move(*a));
            }
            return answers.size() >= count || closed;
        });
        EXPECT_TRUE(done);
        api_->reap();
        return answers;
    }

    Answer ask(std::string_view op, std::string_view body,
               std::string_view token = "service.backend") {
        const int fd = connect();
        auto answers = exchange(fd, request(path(op), body, token));
        // Closed once answered, as a client done with it would: the API holds a few at most.
        clients_.pop_back();
        pending_.erase(fd);
        if (answers.empty()) {
            ADD_FAILURE() << "no answer to " << op;
            return {};
        }
        return answers.front();
    }

    bool peer_closed(int fd) {
        std::string ignored;
        return pump_until(*reactor_, [&] { return read_into(fd, ignored); });
    }

    FakeClock clock_;
    ulw::test::FakeVerifier verifier_;
    Heard heard_;
    FakeCallRooms rooms_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<core::ports::IMessageStore> store_;
    std::unique_ptr<ServiceApi> api_;
    std::uint16_t port_ = 0;
    std::vector<os::UniqueFd> clients_;
    std::map<int, std::string> pending_;
};

// Auth: the same verification as a user's, and then the service claim.
TEST_F(ServiceApiTest, ARequestWithoutAValidBearerTokenIs401AndNothingIsAsked) {
    const std::string body = R"({"users":["alice","bob"]})";
    for (const std::string_view token : {"", "bogus", "user.", "user.bad+user!"}) {
        const Answer a = ask("open_direct", body, token);
        EXPECT_EQ(a.status, 401) << token;
        EXPECT_EQ(a.header("WWW-Authenticate"), "Bearer") << token;
        EXPECT_EQ(a.field("reason"), "unauthorized") << token;
    }
    // A cookie is never read: the backend is not a browser.
    const int fd = connect();
    const auto answers = exchange(fd, request(path("open_direct"), body, "", "POST",
                                              "Cookie: auth_token=service.backend\r\n"));
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 401);
    // A header that is not a bearer token.
    const int basic = connect();
    const auto refused = exchange(
        basic, request(path("open_direct"), body, "", "POST", "Authorization: Basic eA==\r\n"));
    ASSERT_EQ(refused.size(), 1U);
    EXPECT_EQ(refused[0].status, 401);
    EXPECT_EQ(api_->counters().unauthorized, 6U);
    EXPECT_TRUE(heard_.take().empty());
}

TEST_F(ServiceApiTest, AUsersTokenWithoutTheServiceClaimIs403) {
    const Answer a = ask("open_direct", R"({"users":["alice","bob"]})", "user.alice");
    EXPECT_EQ(a.status, 403);
    EXPECT_EQ(a.field("reason"), "forbidden");
    EXPECT_EQ(api_->counters().forbidden, 1U);
    EXPECT_TRUE(heard_.take().empty());
}

TEST_F(ServiceApiTest, KeysThatCannotBeFetchedAre503AndASlowKeyIsWaitedFor) {
    const Answer down = ask("rooms", R"({"user":"alice"})", "down.backend");
    EXPECT_EQ(down.status, 503);
    EXPECT_EQ(down.header("Retry-After"), "1");
    const int fd = connect();
    const std::string bytes = request(path("rooms"), R"({"user":"alice"})", "slow.backend");
    std::size_t sent = 0;
    ASSERT_TRUE(pump_until(*reactor_, [&] {
        sent += ulw::test::write_some(fd, std::as_bytes(std::span(bytes).subspan(sent)));
        return verifier_.waiting() == 1;
    }));
    verifier_.refresh_keys();
    // "slow." tokens verify as users once the keys are in: refused for want of the claim.
    const auto answers = exchange(fd, "");
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 403);
}

TEST_F(ServiceApiTest, ADirectChatIsListedOnceInTheRoomItsPairNames) {
    const Answer a = ask("open_direct", R"({"users":["bob","alice"]})");
    ASSERT_EQ(a.status, 200) << a.body;
    const core::RoomId room = chat::direct_room(user("alice"), user("bob"));
    EXPECT_EQ(a.body, std::format(R"({{"type":"direct","room":"{}","users":["bob","alice"],)"
                                  R"("added":["alice","bob"]}})",
                                  room.to_string()));
    EXPECT_EQ(heard_.take(), (std::vector<std::string>{"+ " + room.to_string() + " alice",
                                                       "+ " + room.to_string() + " bob"}));
    // Again, either way round: the same room, and nobody listed anew.
    const Answer again = ask("open_direct", R"({"users":["alice","bob"]})");
    EXPECT_EQ(again.status, 200);
    EXPECT_EQ(again.field("room"), room.to_string());
    EXPECT_NE(again.body.find(R"("added":[])"), std::string::npos) << again.body;
    EXPECT_TRUE(heard_.take().empty());
    EXPECT_EQ(api_->counters().changes, 2U);
    // A pair of one is not a pair.
    const Answer self = ask("open_direct", R"({"users":["alice","alice"]})");
    EXPECT_EQ(self.status, 400);
    EXPECT_EQ(self.field("reason"), "self");
}

TEST_F(ServiceApiTest, AGroupIsCreatedOnceForItsRequestAndManagedAsItsAdminWould) {
    const Answer created =
        ask("create_group", R"({"creator":"alice","id":"req-1","users":["carol","bob","alice"]})");
    ASSERT_EQ(created.status, 200) << created.body;
    const core::RoomId room = chat::group_room(user("alice"), *rt::MessageKey::parse("req-1"));
    EXPECT_EQ(created.body, std::format(R"({{"type":"group","room":"{}","id":"req-1",)"
                                        R"("added":["alice","bob","carol"]}})",
                                        room.to_string()));
    EXPECT_EQ(heard_.take().size(), 3U);
    const Answer repeat = ask("create_group", R"({"creator":"alice","id":"req-1"})");
    EXPECT_EQ(repeat.status, 200);
    EXPECT_NE(repeat.body.find(R"("added":[])"), std::string::npos);

    const std::string r = room.to_string();
    const Answer added =
        ask("add_members", R"({"room":")" + r + R"(","users":["dave","bob","dave"]})");
    EXPECT_EQ(added.status, 200) << added.body;
    EXPECT_EQ(added.body, std::format(R"({{"type":"added","room":"{}","users":["dave"]}})", r));
    EXPECT_EQ(heard_.take(), std::vector<std::string>{"+ " + r + " dave"});

    const Answer members = ask("members", R"({"room":")" + r + R"(","limit":2})");
    EXPECT_EQ(members.status, 200);
    EXPECT_EQ(members.body,
              std::format(R"({{"type":"members","room":"{}","members":[)"
                          R"({{"user":"alice","role":"admin"}},{{"user":"bob","role":"member"}}],)"
                          R"("more":true}})",
                          r));
    const Answer rest = ask("members", R"({"room":")" + r + R"(","after":"bob"})");
    EXPECT_NE(rest.body.find(R"("more":false)"), std::string::npos) << rest.body;

    const Answer rooms = ask("rooms", R"({"user":"dave"})");
    EXPECT_EQ(rooms.status, 200);
    EXPECT_EQ(rooms.body, std::format(R"({{"type":"rooms","rooms":[{{"room":"{}","kind":"group",)"
                                      R"("role":"member","seq":0}}],"more":false}})",
                                      r));

    // Removing the admin hands the group on, as a leave does.
    const Answer removed = ask("remove_member", R"({"room":")" + r + R"(","user":"alice"})");
    EXPECT_EQ(removed.status, 200) << removed.body;
    EXPECT_EQ(removed.body, std::format(R"({{"type":"removed","room":"{}","user":"alice",)"
                                        R"("removed":["alice"],"promoted":"bob"}})",
                                        r));
    EXPECT_EQ(heard_.take(),
              (std::vector<std::string>{"- " + r + " alice", "* " + r + " admin bob"}));
    const Answer plain = ask("remove_member", R"({"room":")" + r + R"(","user":"carol"})");
    EXPECT_EQ(plain.body, std::format(R"({{"type":"removed","room":"{}","user":"carol",)"
                                      R"("removed":["carol"]}})",
                                      r));
    // Again, after a lost answer say: removed already, so answered alike, removing nobody.
    const Answer again = ask("remove_member", R"({"room":")" + r + R"(","user":"carol"})");
    EXPECT_EQ(again.status, 200);
    EXPECT_EQ(again.body,
              std::format(R"({{"type":"removed","room":"{}","user":"carol","removed":[]}})", r));
    EXPECT_EQ(api_->counters().reads, 3U);
}

// ADR-0123: the backend sets a group member's role, and the group keeps an admin.
TEST_F(ServiceApiTest, TheServiceSetsAMembersRole) {
    ASSERT_EQ(ask("create_group", R"({"creator":"alice","id":"r","users":["bob"]})").status, 200);
    const std::string r = chat::group_room(user("alice"), *rt::MessageKey::parse("r")).to_string();
    heard_.take();
    const Answer promoted =
        ask("set_role", R"({"room":")" + r + R"(","user":"bob","role":"admin"})");
    EXPECT_EQ(promoted.status, 200) << promoted.body;
    EXPECT_EQ(promoted.body, std::format(R"({{"type":"role","room":"{}","user":"bob",)"
                                         R"("role":"admin","changed":true}})",
                                         r));
    EXPECT_EQ(heard_.take(), std::vector<std::string>{"* " + r + " admin bob"});
    // Again, after a lost answer say: nothing changes, and nobody hears of it.
    const Answer again = ask("set_role", R"({"room":")" + r + R"(","user":"bob","role":"admin"})");
    EXPECT_EQ(again.status, 200);
    EXPECT_NE(again.body.find(R"("changed":false)"), std::string::npos) << again.body;
    EXPECT_TRUE(heard_.take().empty());
    ASSERT_EQ(ask("set_role", R"({"room":")" + r + R"(","user":"alice","role":"member"})").status,
              200);
    EXPECT_EQ(heard_.take(), std::vector<std::string>{"* " + r + " member alice"});
    const Answer last = ask("set_role", R"({"room":")" + r + R"(","user":"bob","role":"member"})");
    EXPECT_EQ(last.status, 409);
    EXPECT_EQ(last.field("reason"), "last_admin");
    const Answer stranger =
        ask("set_role", R"({"room":")" + r + R"(","user":"zed","role":"admin"})");
    EXPECT_EQ(stranger.status, 409);
    EXPECT_EQ(stranger.field("reason"), "not_member");
    for (const std::string& body :
         {R"({"room":")" + r + R"(","user":"bob","role":"owner"})",
          R"({"room":")" + r + R"(","user":"bob"})",
          R"({"room":")" + r + R"(","user":"bob","role":"admin","why":1})"}) {
        const Answer bad = ask("set_role", body);
        EXPECT_EQ(bad.status, 400) << body;
        EXPECT_EQ(bad.field("reason"), "malformed") << body;
    }
    // Only a group's own id is a group's.
    ASSERT_EQ(ask("open_direct", R"({"users":["alice","bob"]})").status, 200);
    const std::string direct = chat::direct_room(user("alice"), user("bob")).to_string();
    const Answer not_group =
        ask("set_role", R"({"room":")" + direct + R"(","user":"bob","role":"admin"})");
    EXPECT_EQ(not_group.status, 400);
    EXPECT_EQ(not_group.field("reason"), "bad_room");
}

// ADR-0124: who is connected, and on which devices, from the hook's registry.
// ADR-0132: the user's sockets and calls are cut off on every node, through the store's notice,
// and nobody's list changes; asking again does the same again.
TEST_F(ServiceApiTest, DisconnectUserTellsEveryNodeAndChangesNoList) {
    for (int i = 0; i < 2; ++i) {
        const Answer a = ask("disconnect_user", R"({"user":"alice"})");
        EXPECT_EQ(a.status, 200) << a.body;
        EXPECT_EQ(a.body, R"({"type":"disconnected","user":"alice"})");
        ASSERT_TRUE(pump_until(*reactor_, [&] { return !heard_.changes.empty(); }));
        EXPECT_EQ(heard_.take(), std::vector<std::string>{"! alice"});
    }
    for (const std::string_view body : {R"({})", R"({"user":"alice","x":1})", R"({"user":7})"}) {
        const Answer bad = ask("disconnect_user", body);
        EXPECT_EQ(bad.status, 400) << body;
        EXPECT_EQ(bad.field("reason"), "malformed") << body;
    }
    const Answer bad_user = ask("disconnect_user", R"({"user":"has space"})");
    EXPECT_EQ(bad_user.status, 400);
    EXPECT_EQ(bad_user.field("reason"), "bad_user");
    EXPECT_TRUE(heard_.take().empty());
}

// ADR-0135: the service blanks any stored message, whoever sent it, and the room's members hear
// it through the room plane; a seq with no message is 404, and nothing is told.
TEST_F(ServiceApiTest, RedactMessageBlanksAStoredMessageAndTellsTheRoom) {
    constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
    const core::RoomId room = *core::RoomId::parse(kRoom);
    auto& memory = dynamic_cast<infra::messages::MemoryMessageStore&>(*store_);
    bool stored = false;
    memory.append(
        room, 1, user("bob"), "k1", {std::byte{1}, std::byte{2}}, clock_.wall_now(),
        [&](core::ports::MessageResult<std::uint64_t> r) noexcept { stored = r.has_value(); });
    const std::string body = std::string(R"({"room":")") + std::string(kRoom) + R"(","seq":1})";
    for (int i = 0; i < 2; ++i) {
        const Answer a = ask("redact_message", body);
        EXPECT_EQ(a.status, 200) << a.body;
        EXPECT_EQ(a.body, std::string(R"({"type":"redacted","room":")") + std::string(kRoom) +
                              R"(","seq":1})");
    }
    ASSERT_TRUE(stored);
    ASSERT_EQ(rooms_.notices.size(), 2U);
    EXPECT_EQ(rooms_.notices[0].first, room);
    const auto notice = chat::decode_room_notice(rooms_.notices[0].second);
    ASSERT_TRUE(notice && std::holds_alternative<chat::RedactionNotice>(*notice));
    EXPECT_EQ(std::get<chat::RedactionNotice>(*notice).seq, 1U);
    std::optional<core::ports::MessageResult<std::vector<core::ports::StoredMessage>>> page;
    memory.history_before(room, std::nullopt, 10, [&](auto r) noexcept { page = std::move(r); });
    ASSERT_TRUE(pump_until(*reactor_, [&] { return page.has_value(); }));
    ASSERT_TRUE(*page);
    ASSERT_EQ((*page)->size(), 1U);
    EXPECT_TRUE((*page)->front().redacted);
    EXPECT_TRUE((*page)->front().body.empty());
    // The backend's history says so too (ADR-0133).
    const Answer read =
        ask("history", std::string(R"({"room":")") + std::string(kRoom) + R"(","bodies":true})");
    EXPECT_NE(read.body.find(R"("quiet":false,"redacted":true,"body":"")"), std::string::npos)
        << read.body;

    const Answer missing =
        ask("redact_message", std::string(R"({"room":")") + std::string(kRoom) + R"(","seq":2})");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.field("reason"), "no_message");
    EXPECT_EQ(rooms_.notices.size(), 2U);
    for (const std::string_view bad :
         {R"({})", R"({"room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd"})",
          R"({"room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","seq":0})",
          R"({"room":"01a0eb86-6cca-7dce-84cc-3bb47615f9fd","seq":1,"x":1})"}) {
        const Answer refused = ask("redact_message", bad);
        EXPECT_EQ(refused.status, 400) << bad;
        EXPECT_EQ(refused.field("reason"), "malformed") << bad;
    }
    EXPECT_EQ(ask("redact_message", R"({"room":"nope","seq":1})").field("reason"), "bad_room");
    // Without a room plane, the store's answer alone.
    start(roomy(), false, false);
    EXPECT_EQ(ask("redact_message", body).status, 404);
}

// ADR-0136: how many messages relate to each of some seqs, as the socket's rel_counts answers.
TEST_F(ServiceApiTest, RelCountsSaysHowManyMessagesRelateToEachSeq) {
    constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
    const core::RoomId room = *core::RoomId::parse(kRoom);
    auto& memory = dynamic_cast<infra::messages::MemoryMessageStore&>(*store_);
    int stored = 0;
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        memory.append(
            room, seq, user("bob"), "k" + std::to_string(seq), {std::byte{1}}, clock_.wall_now(),
            [&](core::ports::MessageResult<std::uint64_t> r) noexcept { stored += r ? 1 : 0; },
            false,
            seq == 1
                ? std::nullopt
                : std::optional(core::ports::Relation{
                      .type = seq == 2 ? core::ports::RelType::Thread : core::ports::RelType::Reply,
                      .seq = 1}));
    }
    const std::string at = std::string(R"({"room":")") + std::string(kRoom) + '"';
    const Answer a = ask("rel_counts", at + R"(,"seqs":[1,2],"rel_type":"thread"})");
    EXPECT_EQ(a.status, 200) << a.body;
    EXPECT_EQ(a.body, std::string(R"({"type":"rel_counts","room":")") + std::string(kRoom) +
                          R"(","rel_type":"thread","counts":[{"seq":1,"count":1,"latest":2},)"
                          R"({"seq":2,"count":0}]})");
    EXPECT_EQ(stored, 3);
    // The backend's history has each message's relation too (ADR-0133).
    const Answer read = ask("history", at + R"(,"after":1})");
    EXPECT_NE(read.body.find(R"("quiet":false,"rel":{"type":"thread","seq":1}})"),
              std::string::npos)
        << read.body;
    const Answer replies = ask("rel_counts", at + R"(,"seqs":[1]})");
    EXPECT_NE(replies.body.find(R"({"seq":1,"count":1,"latest":3})"), std::string::npos)
        << replies.body;
    for (const std::string& bad :
         {at + "}", at + R"(,"seqs":[]})", at + R"(,"seqs":["1"]})",
          at + R"(,"seqs":[1],"rel_type":"like"})", at + R"(,"seqs":[1],"x":1})"}) {
        const Answer refused = ask("rel_counts", bad);
        EXPECT_EQ(refused.status, 400) << bad;
        EXPECT_EQ(refused.field("reason"), "malformed") << bad;
    }
    EXPECT_EQ(ask("rel_counts", R"({"room":"x","seqs":[1]})").field("reason"), "bad_room");
}

TEST_F(ServiceApiTest, PresenceSaysWhoIsConnectedAndOnWhichDevices) {
    const Answer off = ask("presence", R"({"users":["alice"]})");
    EXPECT_EQ(off.status, 409);
    EXPECT_EQ(off.field("reason"), "notify_off");

    infra::notify::MemoryConnectivity registry(*reactor_, clock_, core::ports::NotifyTiming{});
    const auto wait = [&](auto call) {
        bool done = false;
        call([&done](auto) noexcept { done = true; });
        ASSERT_TRUE(pump_until(*reactor_, [&] { return done; }));
    };
    wait([&](auto done) { registry.renew(7, std::move(done)); });
    const auto phone = core::DeviceId::parse("01a0eb86-6cca-7dce-84cc-3bb47615f001");
    ASSERT_TRUE(phone);
    wait([&](auto done) {
        registry.sync(7,
                      {{.user = user("alice"), .device = *phone},
                       {.user = user("bob"), .device = std::nullopt}},
                      {}, std::move(done));
    });
    api_->see_connections(&registry);
    const Answer a = ask("presence", R"({"users":["carol","alice","bob","alice"]})");
    EXPECT_EQ(a.status, 200) << a.body;
    EXPECT_EQ(
        a.body,
        R"({"type":"presence","users":[)"
        R"({"user":"carol","connected":false,"devices":[]},)"
        R"({"user":"alice","connected":true,"devices":["01a0eb86-6cca-7dce-84cc-3bb47615f001"]},)"
        R"({"user":"bob","connected":true,"devices":[]}]})");
    std::string many = R"({"users":[)";
    for (int i = 0; i < 65; ++i) {
        many += std::format(R"({}"u{}")", i == 0 ? "" : ",", i);
    }
    many += "]}";
    for (const std::string& body :
         {std::string(R"({"users":[]})"), many, std::string(R"({"users":["alice"],"x":1})")}) {
        const Answer bad = ask("presence", body);
        EXPECT_EQ(bad.status, 400) << body;
        EXPECT_EQ(bad.field("reason"), "malformed") << body;
    }
    api_->see_connections(nullptr);
}

TEST_F(ServiceApiTest, ChangesTheStoreRefusesAreAnsweredWithWhy) {
    ASSERT_EQ(ask("open_direct", R"({"users":["alice","bob"]})").status, 200);
    const std::string direct = chat::direct_room(user("alice"), user("bob")).to_string();
    // A direct chat's pair never changes, and only a group's own id is a group's.
    const Answer not_group =
        ask("add_members", R"({"room":")" + direct + R"(","users":["carol"]})");
    EXPECT_EQ(not_group.status, 400);
    EXPECT_EQ(not_group.field("reason"), "bad_room");
    const Answer v7 = ask("add_members", R"({"room":"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d",)"
                                         R"("users":["carol"]})");
    EXPECT_EQ(v7.status, 400);
    EXPECT_EQ(v7.field("reason"), "bad_room");
    const Answer leave_direct =
        ask("remove_member", R"({"room":")" + direct + R"(","user":"bob"})");
    EXPECT_EQ(leave_direct.status, 409);
    EXPECT_EQ(leave_direct.field("reason"), "not_group");
    const std::string unknown =
        chat::group_room(user("zed"), *rt::MessageKey::parse("nothing")).to_string();
    const Answer no_room = ask("add_members", R"({"room":")" + unknown + R"(","users":["carol"]})");
    EXPECT_EQ(no_room.status, 404);
    EXPECT_EQ(no_room.field("reason"), "no_room");
    const Answer nobody = ask("remove_member", R"({"room":")" + unknown + R"(","user":"carol"})");
    EXPECT_EQ(nobody.status, 200);
    EXPECT_NE(nobody.body.find(R"("removed":[])"), std::string::npos) << nobody.body;
    // A group whose members all left over its history takes nobody new.
    ASSERT_EQ(ask("create_group", R"({"creator":"alice","id":"old"})").status, 200);
    const core::RoomId old = chat::group_room(user("alice"), *rt::MessageKey::parse("old"));
    auto& memory = dynamic_cast<infra::messages::MemoryMessageStore&>(*store_);
    bool stored = false;
    memory.append(
        old, 1, user("alice"), "k1", {std::byte{1}}, clock_.wall_now(),
        [&](core::ports::MessageResult<std::uint64_t> r) noexcept { stored = r.has_value(); });
    ASSERT_EQ(
        ask("remove_member", R"({"room":")" + old.to_string() + R"(","user":"alice"})").status,
        200);
    ASSERT_TRUE(stored);
    const Answer gone =
        ask("add_members", R"({"room":")" + old.to_string() + R"(","users":["bob"]})");
    EXPECT_EQ(gone.status, 409);
    EXPECT_EQ(gone.field("reason"), "gone");
    // The cap holds: 100 members at most, the creator among them.
    ASSERT_EQ(ask("create_group", R"({"creator":"alice","id":"big"})").status, 200);
    const std::string big =
        chat::group_room(user("alice"), *rt::MessageKey::parse("big")).to_string();
    for (int batch = 0; batch < 3; ++batch) {
        std::string users;
        for (int i = 0; i < 50; ++i) {
            users += std::format(R"({}"u{}-{}")", users.empty() ? "" : ",", batch, i);
        }
        std::string request = R"({"room":")" + big + R"(","users":[)";
        request += users;
        request += "]}";
        const Answer a = ask("add_members", request);
        EXPECT_EQ(a.status, batch < 1 ? 200 : 409) << batch << " " << a.body;
        if (batch >= 1) {
            EXPECT_EQ(a.field("reason"), "too_many_members");
        }
    }
    EXPECT_GE(api_->counters().refused, 5U);
}

TEST_F(ServiceApiTest, RequestsThatAreNotTheApisAreRefusedBeforeAnythingIsAsked) {
    const std::string room = chat::direct_room(user("a"), user("b")).to_string();
    std::string fifty_one;
    for (int i = 0; i < 51; ++i) {
        fifty_one += std::format(R"({}"u{}")", fifty_one.empty() ? "" : ",", i);
    }
    const std::vector<std::pair<std::string, std::string>> bad{
        {"open_direct", R"({"users":["alice"]})"},
        {"open_direct", R"({"users":["alice","bob"],"extra":1})"},
        {"open_direct", R"({"users":["alice","bob b"]})"},
        {"open_direct", R"({"users":"alice"})"},
        {"open_direct", R"({"users":["alice",7]})"},
        {"open_direct", R"(["alice","bob"])"},
        {"open_direct", "not json"},
        {"create_group", R"({"creator":"alice"})"},
        {"create_group", R"({"creator":"alice","id":"has space"})"},
        {"create_group", R"({"id":"x"})"},
        {"create_group", R"({"creator":"alice","id":7})"},
        {"add_members", R"({"room":")" + room + R"(","users":[]})"},
        {"add_members", R"({"room":")" + room + R"(","users":[)" + fifty_one + "]}"},
        {"add_members", R"({"room":"not-a-room","users":["x"]})"},
        {"add_members", R"({"room":7,"users":["x"]})"},
        {"add_members", R"({"room":"0245c53c-9c67-87a9-b59f-58fd4886aa2e","users":["x"]})"},
        {"remove_member", R"({"room":")" + room + R"("})"},
        {"remove_member", R"({"room":")" + room + R"(","user":"x","more":1})"},
        {"rooms", R"({"user":"alice","limit":0})"},
        {"rooms", R"({"user":"alice","limit":101})"},
        {"rooms", R"({"user":"alice","after":"nope"})"},
        {"rooms", R"({"user":"alice","other":1})"},
        {"rooms", R"({"user":"bad user"})"},
        {"members", R"({"room":")" + room + R"(","after":"bad user"})"},
        {"members", R"({"room":")" + room + R"(","limit":"5"})"},
        {"members", R"({"room":"nope"})"},
        {"members", R"({"room":")" + room + R"(","x":1})"},
        {"history", R"({"room":")" + room + R"(","before":5,"after":1})"},
        {"history", R"({"room":")" + room + R"(","before":-1})"},
        {"history", R"({"room":")" + room + R"(","after":"1"})"},
        {"history", R"({"room":")" + room + R"(","bodies":"yes"})"},
        {"history", R"({"room":")" + room + R"(","limit":101})"},
        {"history", R"({"room":")" + room + R"(","around":3})"},
        {"history", R"({"after":3})"},
        {"history", R"({"room":")" + chat::presence_room(user("a")).to_string() + R"("})"},
    };
    for (const auto& [op, body] : bad) {
        const Answer a = ask(op, body);
        EXPECT_EQ(a.status, 400) << op << " " << body;
        EXPECT_FALSE(a.field("reason").empty()) << op << " " << body;
    }
    EXPECT_EQ(ask("open_direct", "not json").field("reason"), "not_json");
    EXPECT_EQ(ask("open_direct", R"({"users":["alice","bob b"]})").field("reason"), "bad_user");
    EXPECT_EQ(ask("add_members", R"({"room":"not-a-room","users":["x"]})").field("reason"),
              "bad_room");
    EXPECT_EQ(ask("create_group", R"({"creator":"alice","id":"has space"})").field("reason"),
              "bad_id");
    EXPECT_TRUE(heard_.take().empty());

    // Another path, another method, a body past the bound: refused at the head, connection
    // closed.
    const int fd = connect();
    auto answers = exchange(fd, request("/service/v1/nothing", "{}"));
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 404);
    EXPECT_TRUE(peer_closed(fd));
    const int get = connect();
    answers = exchange(get, request(path("rooms"), "", "service.backend", "GET"));
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 405);
    EXPECT_EQ(answers[0].header("Allow"), "POST");
    const int large = connect();
    answers = exchange(large, request(path("rooms"), std::string(std::size_t{17} * 1024, ' ')));
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 413);
    const int garbage = connect();
    answers = exchange(garbage, "NOT HTTP AT ALL\r\n\r\n");
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 400);
}

TEST_F(ServiceApiTest, OneConnectionCarriesRequestAfterRequestPipelinedOrNot) {
    const int fd = connect();
    auto first = exchange(fd, request(path("open_direct"), R"({"users":["alice","bob"]})"));
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first[0].status, 200);
    EXPECT_EQ(first[0].header("Connection"), "keep-alive");
    const std::string all = request(path("rooms"), R"({"user":"alice"})") +
                            request(path("rooms"), R"({"user":"bob"})") +
                            request(path("rooms"), R"({"user":"carol"})", "user.carol") +
                            request(path("rooms"), R"({"user":"carol"})", "bogus");
    const auto four = exchange(fd, all, 4);
    ASSERT_EQ(four.size(), 4U);
    EXPECT_EQ(four[0].status, 200);
    EXPECT_NE(four[0].body.find(R"("peer":"bob")"), std::string::npos) << four[0].body;
    EXPECT_NE(four[1].body.find(R"("peer":"alice")"), std::string::npos) << four[1].body;
    EXPECT_EQ(four[2].status, 403);
    EXPECT_EQ(four[3].status, 401);
    const auto last = exchange(fd, request(path("rooms"), R"({"user":"dave"})"));
    ASSERT_EQ(last.size(), 1U);
    EXPECT_EQ(last[0].body, R"({"type":"rooms","rooms":[],"more":false})");
    // A request that asks for the connection to close is its last.
    const auto closing = exchange(fd, request(path("rooms"), R"({"user":"dave"})",
                                              "service.backend", "POST", "Connection: close\r\n"));
    ASSERT_EQ(closing.size(), 1U);
    EXPECT_EQ(closing[0].header("Connection"), "close");
    EXPECT_TRUE(peer_closed(fd));
}

TEST_F(ServiceApiTest, PastTheRateRequestsAre429WithWhenToComeBack) {
    ServiceApiLimits limits = roomy();
    limits.burst = 2;
    limits.per_second = 1;
    start(limits);
    EXPECT_EQ(ask("rooms", R"({"user":"alice"})").status, 200);
    EXPECT_EQ(ask("rooms", R"({"user":"alice"})").status, 200);
    const Answer limited = ask("rooms", R"({"user":"alice"})");
    EXPECT_EQ(limited.status, 429);
    EXPECT_EQ(limited.header("Retry-After"), "1");
    EXPECT_EQ(limited.field("reason"), "rate_limited");
    EXPECT_NE(limited.body.find(R"("retry_after_ms":1000)"), std::string::npos) << limited.body;
    clock_.advance(core::Millis{1'000});
    EXPECT_EQ(ask("rooms", R"({"user":"alice"})").status, 200);
    EXPECT_EQ(api_->counters().limited, 1U);
}

TEST_F(ServiceApiTest, AStoreThatCannotBeReachedIs503) {
    start(roomy(), true);
    const std::vector<std::pair<std::string_view, std::string>> requests{
        {"open_direct", R"({"users":["alice","bob"]})"},
        {"create_group", R"({"creator":"alice","id":"g"})"},
        {"add_members", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","users":["b"]})"},
        {"remove_member", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","user":"b"})"},
        {"rooms", R"({"user":"alice"})"},
        {"members", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927"})"},
        {"disconnect_user", R"({"user":"alice"})"},
        {"history", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927"})"},
        {"history", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","after":0})"}};
    for (const auto& [op, body] : requests) {
        const Answer a = ask(op, body);
        EXPECT_EQ(a.status, 503) << op;
        EXPECT_EQ(a.field("reason"), "unavailable") << op;
        EXPECT_EQ(a.header("Retry-After"), "1") << op;
    }
    EXPECT_EQ(api_->counters().unavailable, 9U);
}

// ADR-0133: the backend reads a room's history as the socket's `history` pages it, to check that
// a seq exists and who sent it, with the bodies only when it asks for them.
TEST_F(ServiceApiTest, HistoryNamesEachMessagesSeqSenderIdAndTimeAndItsBodyOnlyWhenAsked) {
    auto& memory = static_cast<infra::messages::MemoryMessageStore&>(*store_);
    const core::RoomId room = chat::direct_room(user("alice"), user("bob"));
    const std::string r = room.to_string();
    const core::WallTime at{std::chrono::microseconds{1767225700123456}};
    int stored = 0;
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        const std::string text = "hi " + std::to_string(seq);
        const auto b = std::as_bytes(std::span{text});
        memory.append(
            room, seq, user(seq == 2 ? "bob" : "alice"), "m" + std::to_string(seq),
            {b.begin(), b.end()}, at + std::chrono::seconds{seq},
            [&](core::ports::MessageResult<std::uint64_t> done) noexcept {
                stored += done ? 1 : 0;
            },
            seq == 3);
    }
    ASSERT_TRUE(pump_until(*reactor_, [&] { return stored == 3; }));

    const Answer newest = ask("history", R"({"room":")" + r + R"(","limit":2})");
    ASSERT_EQ(newest.status, 200) << newest.body;
    EXPECT_EQ(
        newest.body,
        std::format(R"({{"type":"history","room":"{}","messages":[)"
                    R"({{"seq":3,"sender":"alice","id":"m3","at":1767225703123,"quiet":true}},)"
                    R"({{"seq":2,"sender":"bob","id":"m2","at":1767225702123,"quiet":false}}]}})",
                    r));
    const Answer before = ask("history", R"({"room":")" + r + R"(","before":2,"bodies":true})");
    EXPECT_EQ(before.body, std::format(R"({{"type":"history","room":"{}","messages":[)"
                                       R"({{"seq":1,"sender":"alice","id":"m1","at":1767225701123,)"
                                       R"("quiet":false,"body":"aGkgMQ"}}]}})",
                                       r));
    const Answer after = ask("history", R"({"room":")" + r + R"(","after":1,"bodies":false})");
    const auto page = core::json::parse(after.body);
    ASSERT_TRUE(page);
    const auto* messages = page->find("messages")->as_array();
    ASSERT_NE(messages, nullptr);
    ASSERT_EQ(messages->size(), 2U);
    EXPECT_EQ((*messages)[0].find("seq")->as_u64(), 2U);
    EXPECT_EQ((*messages)[1].find("seq")->as_u64(), 3U);
    EXPECT_EQ((*messages)[1].find("body"), nullptr);
    // Past the end, and a room nothing was sent in: an empty page.
    EXPECT_EQ(ask("history", R"({"room":")" + r + R"(","after":3})").body,
              std::format(R"({{"type":"history","room":"{}","messages":[]}})", r));
    const std::string other = chat::direct_room(user("carol"), user("dave")).to_string();
    EXPECT_EQ(ask("history", R"({"room":")" + other + R"("})").body,
              std::format(R"({{"type":"history","room":"{}","messages":[]}})", other));
    EXPECT_EQ(api_->counters().reads, 5U);
}

TEST_F(ServiceApiTest, ConnectionsPastTheCapAndAfterAStopAreClosedAtAccept) {
    ServiceApiLimits limits = roomy();
    limits.max_connections = 1;
    start(limits);
    const int first = connect();
    ASSERT_TRUE(pump_until(*reactor_, [&] { return api_->connections() == 1; }));
    const int second = connect();
    EXPECT_TRUE(peer_closed(second));
    EXPECT_EQ(api_->counters().refused_connections, 1U);
    const auto answers = exchange(first, request(path("rooms"), R"({"user":"alice"})"));
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].status, 200);
    api_->stop();
    EXPECT_TRUE(peer_closed(first));
    const int late = connect();
    EXPECT_TRUE(peer_closed(late));
    api_->reap();
    EXPECT_EQ(api_->connections(), 0U);
}

TEST_F(ServiceApiTest, AConnectionGoneBeforeItsAnswerIsAnsweredNothing) {
    const int fd = connect();
    const std::string bytes = request(path("open_direct"), R"({"users":["alice","bob"]})");
    ASSERT_EQ(ulw::test::write_some(fd, std::as_bytes(std::span(bytes))), bytes.size());
    ASSERT_TRUE(pump_until(*reactor_, [&] { return api_->counters().requests == 1; }));
    clients_.clear();
    ASSERT_TRUE(pump_until(*reactor_, [&] {
        api_->reap();
        return api_->connections() == 0;
    }));
    ulw::test::pump_pending(*reactor_);
    // The change itself stands: the store made it.
    EXPECT_EQ(heard_.take().size(), 2U);
}

TEST_F(ServiceApiTest, AConnectionThatSaysNothingIsClosedSoonAndAnIdleOneAtItsTimeout) {
    ServiceApiLimits limits = roomy();
    limits.first_byte_timeout = core::Millis{1'000};
    limits.idle_timeout = core::Millis{3'000};
    start(limits);
    const int silent = connect();
    const int used = connect();
    ASSERT_EQ(exchange(used, request(path("rooms"), R"({"user":"alice"})")).size(), 1U);
    clock_.advance(core::Millis{1'000});
    EXPECT_TRUE(peer_closed(silent));
    std::string ignored;
    EXPECT_FALSE(read_into(used, ignored)) << "a connection that asked keeps its idle time";
    clock_.advance(core::Millis{2'000});
    EXPECT_TRUE(peer_closed(used));
}

TEST_F(ServiceApiTest, OnePeerAddressHoldsAtMostItsShareOfConnections) {
    ServiceApiLimits limits = roomy();
    limits.max_connections_per_address = 1;
    start(limits);
    const int first = connect();
    ASSERT_TRUE(pump_until(*reactor_, [&] { return api_->connections() == 1; }));
    const int second = connect();
    EXPECT_TRUE(peer_closed(second));
    EXPECT_EQ(api_->counters().refused_connections, 1U);
    EXPECT_EQ(exchange(first, request(path("rooms"), R"({"user":"alice"})")).at(0).status, 200);
}

// Failing tokens spend a small budget of the peer's own, before the service's allowance, which
// only service tokens reach: a stranger can neither spend the backend's rate nor have tokens
// verified without end.
TEST_F(ServiceApiTest, FailedTokensSpendTheirOwnBudgetAndNeverTheServices) {
    ServiceApiLimits limits = roomy();
    limits.failure_burst = 2;
    limits.failures_per_second = 1;
    limits.burst = 3;
    limits.per_second = 1;
    start(limits);
    const int fd = connect();
    const std::string body = R"({"user":"alice"})";
    // Service requests give their failure back: any number of them leave the budget whole.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(exchange(fd, request(path("rooms"), body)).at(0).status, 200) << i;
    }
    EXPECT_EQ(exchange(fd, request(path("rooms"), body)).at(0).status, 429) << "the service's own";
    clock_.advance(core::Millis{1'000});
    const int stranger = connect_from("192.0.2.7");
    EXPECT_EQ(exchange(stranger, request(path("rooms"), body, "bogus")).at(0).status, 401);
    EXPECT_EQ(exchange(stranger, request(path("rooms"), body, "user.mallory")).at(0).status, 403);
    const auto limited = exchange(stranger, request(path("rooms"), body, "service.backend"));
    ASSERT_EQ(limited.size(), 1U);
    EXPECT_EQ(limited[0].status, 429) << "past its failures, nothing is looked at";
    EXPECT_TRUE(peer_closed(stranger));
    EXPECT_EQ(api_->counters().unauthorized, 1U);
    EXPECT_EQ(api_->counters().forbidden, 1U);
    // The service's allowance was not touched by any of it.
    EXPECT_EQ(exchange(fd, request(path("rooms"), body)).at(0).status, 200);
}

// The operator's backend takes a direct chat apart: an unfriend, a block (ADR-0096).
TEST_F(ServiceApiTest, AClosedDirectChatListsNobodyAndClosingAgainChangesNothing) {
    ASSERT_EQ(ask("open_direct", R"({"users":["alice","bob"]})").status, 200);
    heard_.take();
    const std::string room = chat::direct_room(user("alice"), user("bob")).to_string();
    const Answer closed = ask("close_direct", R"({"room":")" + room + R"("})");
    EXPECT_EQ(closed.status, 200) << closed.body;
    EXPECT_EQ(closed.body,
              std::format(R"({{"type":"closed","room":"{}","removed":["alice","bob"]}})", room));
    EXPECT_EQ(heard_.take(),
              (std::vector<std::string>{"- " + room + " alice", "- " + room + " bob"}));
    const Answer again = ask("close_direct", R"({"room":")" + room + R"("})");
    EXPECT_EQ(again.body, std::format(R"({{"type":"closed","room":"{}","removed":[]}})", room));
    EXPECT_EQ(ask("members", R"({"room":")" + room + R"("})").body,
              std::format(R"({{"type":"members","room":"{}","members":[],"more":false}})", room));
    // Only a pair's room is one: a group's id, or anything else, is refused unasked.
    const std::string group =
        chat::group_room(user("alice"), *rt::MessageKey::parse("g")).to_string();
    for (const std::string& bad : {group, std::string("0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d")}) {
        const Answer refused = ask("close_direct", R"({"room":")" + bad + R"("})");
        EXPECT_EQ(refused.status, 400) << bad;
        EXPECT_EQ(refused.field("reason"), "bad_room");
    }
    EXPECT_EQ(ask("close_direct", R"({"room":")" + room + R"(","x":1})").status, 400);
    // Opened again, the pair is listed again.
    EXPECT_NE(
        ask("open_direct", R"({"users":["bob","alice"]})").body.find(R"("added":["alice","bob"])"),
        std::string::npos);
}

// ADR-0111: a call declined for a member, or asked about, from a notification.
constexpr std::string_view kCallRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kCall = "01a0eb86-6cca-7dce-84cc-3bb47615fa00";

std::string call_body(std::string_view user_field = R"(,"user":"bob")") {
    return R"({"room":")" + std::string(kCallRoom) + R"(","call":")" + std::string(kCall) + '"' +
           std::string(user_field) + "}";
}

TEST_F(ServiceApiTest, ADeclineIsTheMembersOwnAskedOfTheRoomsOwnerWhichTheApiThenLeaves) {
    rooms_.reply(chat::CallOutcome::Done);
    const Answer a = ask("decline_call", call_body());
    EXPECT_EQ(a.status, 200) << a.body;
    EXPECT_EQ(a.field("type"), "call_declined");
    EXPECT_EQ(a.field("room"), kCallRoom);
    EXPECT_EQ(a.field("call"), kCall);
    EXPECT_EQ(a.field("by"), "bob");
    ASSERT_TRUE(rooms_.asked);
    const auto* decline = std::get_if<chat::CallSignalRequest>(&*rooms_.asked);
    ASSERT_NE(decline, nullptr);
    EXPECT_EQ(decline->user, user("bob"));
    EXPECT_EQ(decline->signal, chat::CallSignal::Decline);
    EXPECT_EQ(decline->call.to_string(), kCall);
    EXPECT_EQ(rooms_.joined_room, *core::RoomId::parse(kCallRoom));
    EXPECT_EQ(rooms_.joins, 1);
    EXPECT_EQ(rooms_.leaves, 1) << "joined for the ask only";
    EXPECT_EQ(api_->counters().changes, 1U);
}

TEST_F(ServiceApiTest, WhatTheOwnerRefusesIsAnsweredWithWhy) {
    const std::vector<std::tuple<chat::CallOutcome, int, std::string_view>> cases{
        {chat::CallOutcome::NoCall, 409, "no_call"},
        {chat::CallOutcome::NotMember, 409, "not_member"},
        {chat::CallOutcome::NotCallable, 409, "not_callable"},
        {chat::CallOutcome::Disabled, 409, "calls_disabled"},
        {chat::CallOutcome::Busy, 503, "unavailable"},
        {chat::CallOutcome::Unavailable, 503, "unavailable"},
    };
    for (const auto& [outcome, status, reason] : cases) {
        rooms_.reply(outcome);
        const Answer a = ask("decline_call", call_body());
        EXPECT_EQ(a.status, status) << reason;
        EXPECT_EQ(a.field("reason"), reason);
    }
    EXPECT_EQ(rooms_.joins, rooms_.leaves);
    // The owner out of reach, or a join that fails: retry.
    rooms_.answer = std::unexpected(rt::RouteError::Unavailable);
    EXPECT_EQ(ask("decline_call", call_body()).status, 503);
    rooms_.answer = std::vector<std::byte>{std::byte{0}};
    EXPECT_EQ(ask("decline_call", call_body()).status, 503) << "an answer nobody can read";
    rooms_.reply(chat::CallOutcome::Done);
    rooms_.join_result = std::unexpected(rt::RouteError::Unavailable);
    EXPECT_EQ(ask("decline_call", call_body()).status, 503);
    EXPECT_EQ(rooms_.joins, rooms_.leaves);
}

TEST_F(ServiceApiTest, ACallsStatusSaysWhetherItStillRingsForWhomeverItNames) {
    const std::vector<std::pair<chat::CallOutcome, std::string_view>> cases{
        {chat::CallOutcome::Ringing, "ringing"},
        {chat::CallOutcome::Answered, "answered"},
        {chat::CallOutcome::NoCall, "over"},
    };
    for (const auto& [outcome, status] : cases) {
        rooms_.reply(outcome);
        const Answer a = ask("call_status", call_body(""));
        EXPECT_EQ(a.status, 200) << a.body;
        EXPECT_EQ(a.field("type"), "call_status");
        EXPECT_EQ(a.field("status"), status);
        EXPECT_EQ(a.field("call"), kCall);
    }
    ASSERT_TRUE(rooms_.asked);
    const auto* about = std::get_if<chat::CallStatusRequest>(&*rooms_.asked);
    ASSERT_NE(about, nullptr);
    EXPECT_EQ(about->user, std::nullopt);
    rooms_.reply(chat::CallOutcome::Ringing);
    EXPECT_EQ(ask("call_status", call_body()).field("status"), "ringing");
    about = std::get_if<chat::CallStatusRequest>(&*rooms_.asked);
    ASSERT_NE(about, nullptr);
    EXPECT_EQ(about->user, user("bob"));
    rooms_.reply(chat::CallOutcome::Disabled);
    EXPECT_EQ(ask("call_status", call_body()).field("reason"), "calls_disabled");
    EXPECT_EQ(api_->counters().reads, 4U);
}

// ADR-0114: the backend does on a member's behalf what the member's socket can, as for a decline:
// a page that is going away reaches the backend (sendBeacon) when it cannot reach chat.
TEST_F(ServiceApiTest, ADeclineMaySayWhyAndAddAQuickReply) {
    rooms_.reply(chat::CallOutcome::Done);
    const Answer a =
        ask("decline_call", call_body(R"(,"user":"bob","reason":"not_allowed","text":"Later")"));
    EXPECT_EQ(a.status, 200) << a.body;
    EXPECT_EQ(a.field("reason"), "not_allowed");
    EXPECT_EQ(a.field("text"), "Later");
    const auto* asked = std::get_if<chat::CallSignalRequest>(&*rooms_.asked);
    ASSERT_NE(asked, nullptr);
    EXPECT_EQ(asked->reason, chat::CallReason::NotAllowed);
    EXPECT_EQ(asked->text, "Later");
    EXPECT_EQ(ask("decline_call", call_body(R"(,"user":"bob","reason":"caller_left")")).status,
              400);
    EXPECT_EQ(ask("cancel_call", call_body(R"(,"user":"bob","reason":"busy")")).status, 400);
}

TEST_F(ServiceApiTest, ACancelAnEndAndALeaveAreTheMembersOwnAskedOfTheRoomsOwner) {
    const std::vector<std::tuple<std::string_view, chat::CallSignal, std::string_view>> cases{
        {"cancel_call", chat::CallSignal::Cancel, "call_cancelled"},
        {"end_call", chat::CallSignal::End, "call_ended"},
        {"leave_call", chat::CallSignal::Leave, "call_left"},
    };
    for (const auto& [op, signal, type] : cases) {
        rooms_.reply(chat::CallOutcome::Done);
        const Answer a = ask(op, call_body());
        EXPECT_EQ(a.status, 200) << op << " " << a.body;
        EXPECT_EQ(a.field("type"), type);
        EXPECT_EQ(a.field("call"), kCall);
        EXPECT_EQ(a.field("by"), "bob");
        ASSERT_TRUE(rooms_.asked);
        const auto* asked = std::get_if<chat::CallSignalRequest>(&*rooms_.asked);
        ASSERT_NE(asked, nullptr) << op;
        EXPECT_EQ(asked->signal, signal);
        EXPECT_EQ(asked->user, user("bob"));
        // Not the call's state for it, as the member's socket would be told.
        rooms_.reply(chat::CallOutcome::NoCall);
        EXPECT_EQ(ask(op, call_body()).field("reason"), "no_call");
        EXPECT_EQ(ask(op, call_body("")).field("reason"), "malformed") << op << " names its user";
    }
    EXPECT_EQ(rooms_.joins, rooms_.leaves);
    EXPECT_EQ(api_->counters().changes, 3U);
}

TEST_F(ServiceApiTest, ACallsStatusWithoutItsIdIsTheRoomsCallAndWhoIsInIt) {
    const chat::CallSnapshot snapshot{
        .call = *chat::CallId::parse(kCall),
        .kind = chat::CallKind::Group,
        .caller = user("alice"),
        .answered = true,
        .ringing = {user("carol")},
        .joined = {user("alice"), user("bob")},
        .connected = std::vector{user("alice")},
        .started_at = core::WallTime{std::chrono::seconds{1'790'000'000}},
        .answered_at = core::WallTime{std::chrono::seconds{1'790'000'007}},
        .expires_at = core::WallTime{std::chrono::seconds{1'790'000'045}}};
    rooms_.answer = chat::encode_answer(
        {.outcome = chat::CallOutcome::Current, .ticket = std::nullopt, .snapshot = snapshot});
    const std::string room_only = R"({"room":")" + std::string(kCallRoom) + R"("})";
    const Answer a = ask("call_status", room_only);
    EXPECT_EQ(a.status, 200) << a.body;
    EXPECT_EQ(a.body, std::format(R"({{"type":"call_status","room":"{}","call":"{}",)"
                                  R"("status":"answered","kind":"group","caller":"alice",)"
                                  R"("ringing":["carol"],"joined":["alice","bob"],)"
                                  R"("connected":["alice"],"started_at":1790000000,)"
                                  R"("answered_at":1790000007,"expires_at":1790000045}})",
                                  kCallRoom, kCall));
    ASSERT_TRUE(rooms_.asked);
    EXPECT_TRUE(std::holds_alternative<chat::CallCurrentRequest>(*rooms_.asked));
    rooms_.reply(chat::CallOutcome::NoCall);
    const Answer none = ask("call_status", room_only);
    EXPECT_EQ(none.status, 200);
    EXPECT_EQ(none.field("status"), "none");
    EXPECT_EQ(none.body.find(R"("call":null)") != std::string::npos, true);
    // A user names whom a ring rings, which only an id asks about.
    EXPECT_EQ(ask("call_status", R"({"room":")" + std::string(kCallRoom) + R"(","user":"bob"})")
                  .field("reason"),
              "malformed");
    EXPECT_EQ(api_->counters().reads, 2U);
}

TEST_F(ServiceApiTest, ACallAskThatIsNotOneOfTheApisIsRefusedBeforeAnythingIsAsked) {
    const std::string live = chat::live_chat_room("stream-1").value().to_string();
    const std::vector<std::pair<std::string, std::string_view>> cases{
        {R"({"room":")" + std::string(kCallRoom) + R"(","user":"bob"})", "malformed"},
        {R"({"room":")" + std::string(kCallRoom) + R"(","call":"x","user":"bob"})", "bad_call"},
        {R"({"room":")" + live + R"(","call":")" + std::string(kCall) + R"(","user":"bob"})",
         "bad_room"},
        {R"({"room":")" + std::string(kCallRoom) + R"(","call":")" + std::string(kCall) +
             R"(","user":"b o b"})",
         "bad_user"},
        {call_body(R"(,"user":"bob","extra":1)"), "malformed"},
    };
    for (const auto& [body, reason] : cases) {
        for (const std::string_view op :
             {"decline_call", "cancel_call", "end_call", "leave_call", "call_status"}) {
            const Answer a = ask(op, body);
            EXPECT_EQ(a.status, 400) << op << " " << body;
            EXPECT_EQ(a.field("reason"), reason) << op << " " << body;
        }
    }
    EXPECT_EQ(ask("decline_call", call_body("")).field("reason"), "malformed")
        << "a decline names its user";
    EXPECT_EQ(rooms_.joins, 0);
}

TEST_F(ServiceApiTest, WithoutTheRoomPlaneACallAskIsUnavailable) {
    start(roomy(), /*down=*/false, /*with_rooms=*/false);
    const Answer a = ask("call_status", call_body());
    EXPECT_EQ(a.status, 503);
    EXPECT_EQ(a.field("reason"), "unavailable");
}

TEST_F(ServiceApiTest, AConnectionClosedBeforeTheOwnerAnswersLeavesTheRoom) {
    rooms_.hold = true;
    const int fd = connect();
    const std::string bytes = request(path("decline_call"), call_body());
    ASSERT_EQ(ulw::test::write_some(fd, std::as_bytes(std::span(bytes))), bytes.size());
    ASSERT_TRUE(pump_until(*reactor_, [&] { return static_cast<bool>(rooms_.held); }));
    // The request's time runs out (a request is not read further while it is answered): the
    // connection closes, and the ask goes with the room it joined.
    clock_.advance(ServiceApiLimits{}.request_timeout + core::Millis{200});
    ASSERT_TRUE(pump_until(*reactor_, [&] {
        api_->reap();
        return rooms_.leaves == 1;
    }));
    EXPECT_FALSE(rooms_.held) << "the ask was dropped with the leave";
    EXPECT_TRUE(peer_closed(fd));
}

// ADR-0138: reads answers every member's read mark in a room, as the socket's reads does; a
// store that cannot be reached is 503.
TEST_F(ServiceApiTest, ReadsAnswersEveryMembersReadMark) {
    ASSERT_EQ(ask("create_group", R"({"creator":"alice","id":"seen","users":["bob"]})").status,
              200);
    const core::RoomId room = chat::group_room(user("alice"), *rt::MessageKey::parse("seen"));
    const std::string r = room.to_string();
    EXPECT_EQ(ask("reads", R"({"room":")" + r + R"("})").body,
              std::format(R"({{"type":"reads","room":"{}","reads":[]}})", r));
    auto& memory = dynamic_cast<infra::messages::MemoryMessageStore&>(*store_);
    memory.append(room, 1, user("alice"), "k1", {std::byte{1}}, clock_.wall_now(),
                  [](core::ports::MessageResult<std::uint64_t> /*stored*/) noexcept {});
    std::optional<std::uint64_t> moved;
    memory.mark_read(room, user("bob"), 4,
                     [&](core::ports::MessageResult<std::optional<std::uint64_t>> m) noexcept {
                         moved = m.value_or(std::nullopt);
                     });
    const Answer reads = ask("reads", R"({"room":")" + r + R"("})");
    EXPECT_EQ(moved, 1U);
    EXPECT_EQ(reads.status, 200);
    EXPECT_EQ(reads.body, std::format(R"({{"type":"reads","room":"{}","reads":[)"
                                      R"({{"user":"bob","seq":1}}]}})",
                                      r));
    EXPECT_EQ(ask("reads", R"({"room":")" + r + R"(","user":"bob"})").status, 400);
    EXPECT_EQ(ask("reads", R"({"room":"nope"})").status, 400);
    start(roomy(), true);
    EXPECT_EQ(ask("reads", R"({"room":")" + r + R"("})").status, 503);
}

} // namespace
