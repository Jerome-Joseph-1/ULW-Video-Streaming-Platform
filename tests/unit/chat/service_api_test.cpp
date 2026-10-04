// Chat's service API (ADR-0096): the operator's backend lists and unlists members, authenticated
// by the identity provider's token with the service claim, through the store's own changes.
#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"

#include "named_rooms.hpp"
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

class ServiceApiTest : public ::testing::Test {
protected:
    void SetUp() override { start({}); }

    void TearDown() override {
        clients_.clear();
        // The store first, as the API's contract asks.
        store_.reset();
        api_.reset();
        reactor_.reset();
    }

    void start(const ServiceApiLimits& limits, bool down = false) {
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
        api_ = std::make_unique<ServiceApi>(*reactor_, clock_, verifier_, *store_, limits);
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
                                      R"("role":"member"}}],"more":false}})",
                                      r));

    // Removing the admin hands the group on, as a leave does.
    const Answer removed = ask("remove_member", R"({"room":")" + r + R"(","user":"alice"})");
    EXPECT_EQ(removed.status, 200) << removed.body;
    EXPECT_EQ(
        removed.body,
        std::format(R"({{"type":"removed","room":"{}","user":"alice","promoted":"bob"}})", r));
    EXPECT_EQ(heard_.take(),
              (std::vector<std::string>{"- " + r + " alice", "* " + r + " admin bob"}));
    const Answer plain = ask("remove_member", R"({"room":")" + r + R"(","user":"carol"})");
    EXPECT_EQ(plain.body, std::format(R"({{"type":"removed","room":"{}","user":"carol"}})", r));
    EXPECT_EQ(api_->counters().reads, 3U);
}

TEST_F(ServiceApiTest, ChangesTheStoreRefusesAreAnsweredWithWhy) {
    ASSERT_EQ(ask("open_direct", R"({"users":["alice","bob"]})").status, 200);
    const std::string direct = chat::direct_room(user("alice"), user("bob")).to_string();
    const Answer not_group =
        ask("add_members", R"({"room":")" + direct + R"(","users":["carol"]})");
    EXPECT_EQ(not_group.status, 409);
    EXPECT_EQ(not_group.field("reason"), "not_group");
    const Answer leave_direct =
        ask("remove_member", R"({"room":")" + direct + R"(","user":"bob"})");
    EXPECT_EQ(leave_direct.status, 409);
    EXPECT_EQ(leave_direct.field("reason"), "not_group");
    const std::string unknown =
        chat::group_room(user("zed"), *rt::MessageKey::parse("nothing")).to_string();
    const Answer no_room = ask("add_members", R"({"room":")" + unknown + R"(","users":["carol"]})");
    EXPECT_EQ(no_room.status, 404);
    EXPECT_EQ(no_room.field("reason"), "no_room");
    const Answer not_member =
        ask("remove_member", R"({"room":")" + unknown + R"(","user":"carol"})");
    EXPECT_EQ(not_member.status, 404);
    EXPECT_EQ(not_member.field("reason"), "not_member");
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
    start({.burst = 2, .per_second = 1});
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
    start({}, true);
    const std::vector<std::pair<std::string_view, std::string>> requests{
        {"open_direct", R"({"users":["alice","bob"]})"},
        {"create_group", R"({"creator":"alice","id":"g"})"},
        {"add_members", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","users":["b"]})"},
        {"remove_member", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927","user":"b"})"},
        {"rooms", R"({"user":"alice"})"},
        {"members", R"({"room":"04ff0817-879c-8a63-b5a8-9ebacad5d927"})"}};
    for (const auto& [op, body] : requests) {
        const Answer a = ask(op, body);
        EXPECT_EQ(a.status, 503) << op;
        EXPECT_EQ(a.field("reason"), "unavailable") << op;
        EXPECT_EQ(a.header("Retry-After"), "1") << op;
    }
    EXPECT_EQ(api_->counters().unavailable, 6U);
}

TEST_F(ServiceApiTest, ConnectionsPastTheCapAndAfterAStopAreClosedAtAccept) {
    start({.max_connections = 1});
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

TEST_F(ServiceApiTest, AnIdleConnectionIsClosedAtItsTimeout) {
    start({.idle_timeout = core::Millis{1'000}});
    const int fd = connect();
    ASSERT_TRUE(pump_until(*reactor_, [&] { return api_->connections() == 1; }));
    clock_.advance(core::Millis{1'000});
    EXPECT_TRUE(peer_closed(fd));
}

} // namespace
