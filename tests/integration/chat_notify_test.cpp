// The notification hook as chat_server sends it (ADR-0111), on the three-node cluster: the
// events a ring and a message make reach the operator's receiver, with the devices connected
// anywhere; a socket opened after the ring answers it by its id; the service API declines a call
// for a member and says whether one still rings. Needs a LiveKit, as the tickets do.

#include "chat_cluster.hpp"
#include "support/http_test_server.hpp"

#include <chrono>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using ulw::test::Client;
using ulw::test::Seen;

constexpr std::string_view kSecret = "a-test-only-hook-secret-0123456789abcdef";
constexpr std::string_view kPhone = "01a0eb86-6cca-7dce-84cc-3bb47615f9b1";
constexpr std::string_view kLaptop = "01a0eb86-6cca-7dce-84cc-3bb47615f9b2";
constexpr std::string_view kAliceDevice = "01a0eb86-6cca-7dce-84cc-3bb47615f9a1";

std::string field(const core::json::Value& v, std::string_view key) {
    const core::json::Value* f = v.find(key);
    return f == nullptr ? std::string{} : std::string(f->as_string().value_or(""));
}

std::vector<std::string> strings(const core::json::Value* v) {
    std::vector<std::string> out;
    if (v == nullptr || v->as_array() == nullptr) {
        return out;
    }
    for (const core::json::Value& item : *v->as_array()) {
        out.emplace_back(item.as_string().value_or(""));
    }
    return out;
}

class ChatNotifyTest : public ulw::test::ChatCluster {
protected:
    ChatNotifyTest()
        : receiver_([](const ulw::test::ServedRequest&) {
              return ulw::test::Reply{.status = 204, .headers = {}, .body = {}};
          }) {}

    [[nodiscard]] std::vector<std::string> extra_env() const override {
        return {"ULW_NOTIFY_URL=" + receiver_.base_url() + "/ulw/events",
                "ULW_NOTIFY_SECRET=" + std::string(kSecret),
                "ULW_NOTIFY_MESSAGE_INTERVAL_SECONDS=5"};
    }

    // Every event the receiver was sent, in the order its requests came.
    [[nodiscard]] std::vector<core::json::Value> events() const {
        std::vector<core::json::Value> out;
        for (const ulw::test::ServedRequest& r : receiver_.requests()) {
            EXPECT_EQ(r.method, "POST");
            EXPECT_EQ(r.path(), "/ulw/events");
            const auto signature = r.header("ulw-signature");
            EXPECT_TRUE(signature && signature->starts_with("t=") &&
                        signature->find(",v1=") != std::string_view::npos);
            auto body = core::json::parse(r.body);
            EXPECT_TRUE(body) << r.body;
            if (!body || body->find("events") == nullptr ||
                body->find("events")->as_array() == nullptr) {
                continue;
            }
            for (const core::json::Value& e : *body->find("events")->as_array()) {
                out.push_back(e);
            }
        }
        return out;
    }

    // The first event of `type` for `call` (or any, when empty), waited for.
    std::optional<core::json::Value>
    event(const std::string& type, const std::string& call = {},
          std::chrono::milliseconds limit = std::chrono::seconds(10)) const {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            for (core::json::Value& e : events()) {
                if (field(e, "type") == type && (call.empty() || field(e, "call") == call)) {
                    return std::move(e);
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                ADD_FAILURE() << "the receiver was never sent " << type;
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    [[nodiscard]] std::size_t count(const std::string& type) const {
        std::size_t n = 0;
        for (const core::json::Value& e : events()) {
            n += field(e, "type") == type ? std::size_t{1} : std::size_t{0};
        }
        return n;
    }

    // A call's ticket, or why not, for `client`.
    static std::optional<Seen> call(Client& client, const std::string& room,
                                    std::string_view device, const std::string& answering = {}) {
        const std::size_t from = client.seen().size();
        const std::string answer =
            answering.empty() ? std::string{} : R"(,"answer":")" + answering + '"';
        if (!client.send(R"({"type":"call","room":")" + room + R"(","device":")" +
                         std::string(device) + '"' + answer + "}")) {
            return std::nullopt;
        }
        const auto at = client.wait_from(from, [&](const Seen& s) {
            return s.room == room && (s.type == "ticket" || s.type == "error");
        });
        if (!at) {
            return std::nullopt;
        }
        return client.seen()[*at];
    }

    // The service API's answer, as status and parsed body.
    std::pair<int, core::json::Value> service(std::size_t node, std::string_view op,
                                              const std::string& body) const {
        const auto answer =
            ulw::test::service_post(nodes_[node].service_port, op, body, mint_service());
        auto json = core::json::parse(answer.body);
        EXPECT_TRUE(json) << answer.status << " " << answer.body;
        return {answer.status, json ? std::move(*json) : core::json::Value{}};
    }

    std::string status_of(std::size_t node, const std::string& room, const std::string& call,
                          const std::string& user = {}) const {
        const std::string who = user.empty() ? std::string{} : R"(,"user":")" + user + '"';
        const auto [status, body] = service(
            node, "call_status", R"({"room":")" + room + R"(","call":")" + call + '"' + who + "}");
        EXPECT_EQ(status, 200) << field(body, "reason");
        EXPECT_EQ(field(body, "type"), "call_status");
        return field(body, "status");
    }

    static std::string path_of(std::string_view device) {
        return "/rt?device=" + std::string(device);
    }

    ulw::test::HttpTestServer receiver_;
};

TEST_P(ChatNotifyTest, ARingReachesTheReceiverWithTheDevicesConnectedAnywhereAndHowItEnded) {
    if (ulw::test::livekit_environment().empty()) {
        GTEST_SKIP() << "no LiveKit: set LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY "
                        "and LIVEKIT_API_SECRET";
    }
    const std::string direct = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(direct, {"alice", "bob"}, "direct_chat"));
    auto alice = connect(nodes_[0], 0, path_of(kAliceDevice));
    auto phone = connect(nodes_[1], 1, path_of(kPhone));
    ASSERT_TRUE(alice && phone);
    ASSERT_EQ(join_answer(*alice, direct), "joined");
    // The registry is written a moment after a socket opens.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 1. alice rings bob: the owner tells the receiver which of bob's devices are connected.
    const auto ticket = call(*alice, direct, kAliceDevice);
    ASSERT_TRUE(ticket);
    ASSERT_EQ(ticket->type, "ticket") << ticket->reason;
    const std::string first = ticket->call;
    const auto ringing = event("call_ringing", first);
    ASSERT_TRUE(ringing);
    EXPECT_EQ(field(*ringing, "room"), direct);
    EXPECT_EQ(field(*ringing, "kind"), "direct");
    EXPECT_EQ(field(*ringing, "caller"), "alice");
    EXPECT_FALSE(field(*ringing, "id").empty());
    ASSERT_NE(ringing->find("callees"), nullptr);
    const auto* callees = ringing->find("callees")->as_array();
    ASSERT_TRUE(callees != nullptr && callees->size() == 1);
    EXPECT_EQ(field((*callees)[0], "user"), "bob");
    EXPECT_EQ((*callees)[0].find("connected")->as_bool(), true);
    EXPECT_EQ(strings((*callees)[0].find("devices")),
              std::vector<std::string>{std::string(kPhone)});
    // Asked of any node, the owner answers.
    EXPECT_EQ(status_of(2, direct, first), "ringing");
    EXPECT_EQ(status_of(1, direct, first, "bob"), "ringing");

    // 2. bob's phone goes; the notification opens a socket on another node after the ring, which
    // answers the call by its id.
    phone.reset();
    auto laptop = connect(nodes_[2], 1, path_of(kLaptop));
    ASSERT_TRUE(laptop);
    ASSERT_EQ(join_answer(*laptop, direct), "joined");
    const auto answered = call(*laptop, direct, kLaptop, first);
    ASSERT_TRUE(answered);
    ASSERT_EQ(answered->type, "ticket") << answered->reason;
    EXPECT_EQ(answered->call, first);
    const auto answered_event = event("call_answered", first);
    ASSERT_TRUE(answered_event);
    EXPECT_EQ(field(*answered_event, "by"), "bob");
    EXPECT_EQ(status_of(0, direct, first), "answered");
    ASSERT_TRUE(
        alice->send(R"({"type":"call_end","room":")" + direct + R"(","call":")" + first + R"("})"));
    const auto ended = event("call_ended", first);
    ASSERT_TRUE(ended);
    EXPECT_EQ(field(*ended, "by"), "alice");
    EXPECT_EQ(status_of(1, direct, first), "over");

    // 3. alice rings again, once the registry has the phone gone and the laptop come; the service
    // API declines it for bob, as his socket would.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto again = call(*alice, direct, kAliceDevice);
    ASSERT_TRUE(again);
    ASSERT_EQ(again->type, "ticket") << again->reason;
    const std::string second = again->call;
    const auto rung = event("call_ringing", second);
    ASSERT_TRUE(rung);
    EXPECT_EQ(strings((*rung->find("callees")->as_array())[0].find("devices")),
              std::vector<std::string>{std::string(kLaptop)});
    const std::string decline =
        R"({"room":")" + direct + R"(","call":")" + second + R"(","user":"bob"})";
    // Not on the room's list: refused, and nothing changes.
    const auto [stranger, refused] =
        service(1, "decline_call",
                R"({"room":")" + direct + R"(","call":")" + second + R"(","user":"carol"})");
    EXPECT_EQ(stranger, 409);
    EXPECT_EQ(field(refused, "reason"), "not_member");
    const auto [declined_status, declined] = service(1, "decline_call", decline);
    EXPECT_EQ(declined_status, 200) << field(declined, "reason");
    EXPECT_EQ(field(declined, "type"), "call_declined");
    EXPECT_EQ(field(declined, "by"), "bob");
    const std::size_t from = alice->seen().size();
    ASSERT_TRUE(alice->wait_from(from == 0 ? 0 : from - 1, [&](const Seen& s) {
        return s.type == "call_declined" && s.call == second && s.by == "bob";
    }));
    const auto declined_event = event("call_declined", second);
    ASSERT_TRUE(declined_event);
    EXPECT_EQ(field(*declined_event, "by"), "bob");
    const auto [twice, again_refused] = service(2, "decline_call", decline);
    EXPECT_EQ(twice, 409);
    EXPECT_EQ(field(again_refused, "reason"), "no_call");
    EXPECT_EQ(status_of(2, direct, second), "over");
    EXPECT_GE(metric(nodes_[0], "notify_events_total{outcome=\"sent\"}") +
                  metric(nodes_[1], "notify_events_total{outcome=\"sent\"}") +
                  metric(nodes_[2], "notify_events_total{outcome=\"sent\"}"),
              5U)
        << "two rings, an answer, an end and a decline";
}

TEST_P(ChatNotifyTest, AMessageNamesTheMembersWithNoSocketOncePerIntervalAndNeverItsBody) {
    const std::string group = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(group, {"alice", "bob", "carol"}, "group_chat"));
    auto alice = connect(nodes_[0], 0);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && carol);
    ASSERT_EQ(join_answer(*alice, group), "joined");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_TRUE(alice->send(send_command(group, "secret words", "n1")));
    ASSERT_TRUE(alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "n1"; }));
    const auto told = event("message");
    ASSERT_TRUE(told);
    EXPECT_EQ(field(*told, "room"), group);
    EXPECT_EQ(field(*told, "sender"), "alice");
    EXPECT_EQ(strings(told->find("users")), std::vector<std::string>{"bob"});
    ASSERT_NE(told->find("seq"), nullptr);
    EXPECT_GE(told->find("seq")->as_u64().value_or(0), 1U);
    for (const ulw::test::ServedRequest& r : receiver_.requests()) {
        EXPECT_EQ(r.body.find("secret words"), std::string::npos) << "never content";
    }
    // Within the interval, nobody is told again.
    ASSERT_TRUE(alice->send(send_command(group, "more", "n2")));
    ASSERT_TRUE(alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "n2"; }));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_EQ(count("message"), 1U);
    // bob connects: he is no longer owed anything, and the next time he goes he starts afresh.
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(bob);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_TRUE(alice->send(send_command(group, "while bob is here", "n3")));
    ASSERT_TRUE(alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "n3"; }));
    bob.reset();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_EQ(count("message"), 1U);
    ASSERT_TRUE(alice->send(send_command(group, "bob has gone", "n4")));
    ASSERT_TRUE(alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "n4"; }));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_EQ(count("message"), 2U);
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatNotifyTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
