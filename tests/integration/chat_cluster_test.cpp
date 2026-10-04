// The M16 acceptance: three chat_server processes on one Postgres. Clients on different nodes
// see each other's messages; a stopped owner is replaced under a higher owner_generation; once
// resumed, its stale write updates no rows, is logged as "fenced out", and reaches nobody.
// The M17 acceptance on the same cluster: clients on every node see one order by last_seq; a
// rate-limited send is refused and reaches nobody; a repeated send is delivered once; a client
// that comes back resumes from its last seq; no body shows in any log or in the database.
// The M19 acceptance: every message is stored with its seq, as the bytes that were sent and in
// no readable form besides; history survives a restart of every node, in order; a client that
// resumes through a node that kept nothing fills the gap from history; a room with members
// refuses anyone else.
// The M18 acceptance: a user who reconnects within the grace is no event to anyone watching; one
// who does not is exactly one offline on every watching node; a user nobody watches costs no
// presence message at all.
// The M32 acceptance: a stream's live chat on the same cluster. A viewer that all but stops
// reading holds its node to no more memory and is counted as dropping; every other viewer, on
// every node, gets every message in order.

#include "chat_cluster.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using infra::postgres::Params;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::Client;
using ulw::test::kGrace;
using ulw::test::kReadyCheckPeriod;
using ulw::test::Node;
using ulw::test::Seen;

class ChatClusterTest : public ulw::test::ChatCluster {
protected:
    // How often a send answered `unavailable` goes again before the answer is taken as final.
    static constexpr int kSendAttempts = 20;

    // The last messages `client` was sent, by type, id, seq and reason (never a body), and what
    // every node wrote: a failure then tells an answer the test did not expect from silence.
    [[nodiscard]] std::string what_was_seen(const Client& client) const {
        constexpr std::size_t kLast = 10;
        const std::vector<Seen>& seen = client.seen();
        const std::size_t shown = std::min(seen.size(), kLast);
        std::string out = std::format("\n{} was sent {} messages; the last {}:", client.name(),
                                      seen.size(), shown);
        for (std::size_t i = seen.size() - shown; i < seen.size(); ++i) {
            out += std::format("\n  {} id={} seq={} reason={}", seen[i].type, seen[i].id,
                               seen[i].seq, seen[i].reason);
        }
        for (const Node& n : nodes_) {
            // A resend answered from a node's memory counts as deduplicated; an `unavailable`
            // that came from a forward, not the store, as a forward timeout.
            const std::string metrics = ulw::test::http_get(n.port, "/metrics").body;
            out += std::format("\n{}: messages_deduplicated_total {}, forward_timeouts_total {}; "
                               "it wrote:\n{}",
                               n.name, counter(metrics, "messages_deduplicated_total"),
                               counter(metrics, "forward_timeouts_total"), n.process->output());
        }
        return out;
    }

    // A counter's value in a /metrics page, as text; "absent" when the page does not have it.
    [[nodiscard]] static std::string counter(std::string_view metrics, std::string_view name) {
        const std::string line = "\n" + std::string(name) + " ";
        const std::size_t at = metrics.find(line);
        if (at == std::string_view::npos) {
            return "absent";
        }
        const std::size_t start = at + line.size();
        return std::string(metrics.substr(start, metrics.find('\n', start) - start));
    }

    // Sends `body` under `id` as a client must (ADR-0043): `unavailable` leaves the send's fate
    // unknown, so it goes again under the same id, each time once the last is answered. Returns
    // the first other answer, or the last `unavailable` after kSendAttempts; nullopt when a send
    // was not answered at all.
    // Each resend goes out as soon as the last answer is in, with no pause between. A run of
    // them that outpaces the sender's allowance is answered `rate_limited`, which is returned
    // like any other answer: the caller's check for `sent` then fails loudly with that reason,
    // never passes by mistake.
    [[nodiscard]] std::optional<Seen> send_until_answered(Client& client, const std::string& body,
                                                          const std::string& id) {
        const auto is_answer = [&](const Seen& s) {
            return (s.type == "sent" || s.type == "error") && s.id == id;
        };
        std::optional<Seen> answer;
        for (int attempt = 0; attempt < kSendAttempts; ++attempt) {
            const std::size_t answers = client.count(is_answer);
            if (!client.send(send_command(room_, body, id))) {
                ADD_FAILURE() << client.name() << " could not send " << id;
                return std::nullopt;
            }
            if (!client.wait_for([&](const Seen&) { return client.count(is_answer) > answers; })) {
                return std::nullopt;
            }
            answer = *std::ranges::find_last_if(client.seen(), is_answer).begin();
            if (answer->type != "error" || answer->reason != "unavailable") {
                return answer;
            }
        }
        return answer;
    }
};

TEST_P(ChatClusterTest, AStoppedOwnerIsReplacedAndItsLateWriteIsFencedOutAndDeliveredNowhere) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && bob && carol);
    // chat-1 resolves the room first, and so creates it and owns it.
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    ASSERT_NO_FATAL_FAILURE(join(*carol));
    ASSERT_EQ(owner(), "chat-1 1");

    // Clients on different nodes see each other's messages, each under the one seq its owner
    // gave it.
    ASSERT_TRUE(bob->send(send_command(room_, "hello from chat-2", ref(1))));
    ASSERT_TRUE(alice->send(send_command(room_, "hello from chat-1", ref(2))));
    std::optional<std::uint64_t> from_bob;
    std::optional<std::uint64_t> from_alice;
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        const auto first = c->message("hello from chat-2");
        const auto second = c->message("hello from chat-1");
        ASSERT_TRUE(first && second) << c->name();
        EXPECT_EQ(first->sender, "bob");
        EXPECT_EQ(second->sender, "alice");
        EXPECT_EQ(first->seq, from_bob.value_or(first->seq)) << c->name();
        EXPECT_EQ(second->seq, from_alice.value_or(second->seq)) << c->name();
        from_bob = first->seq;
        from_alice = second->seq;
    }
    EXPECT_EQ(*from_bob + *from_alice, 3U) << "the two messages are seqs 1 and 2";

    // Stop the owner, and give it a write to make once it runs again.
    auto watch = db_->session();
    ASSERT_TRUE(watch.exec("LISTEN room_owner"));
    Node& stalled = nodes_[0];
    stalled.process->signal(SIGSTOP);
    ASSERT_TRUE(alice->send(send_command(room_, "stale", ref(7))));

    // A survivor claims the room under a higher generation.
    const auto deadline = std::chrono::steady_clock::now() + seconds(30);
    std::string now_owned = owner();
    while (now_owned == "chat-1 1" && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(watch.wait_for_notification(core::Millis{1'000}));
        now_owned = owner();
    }
    ASSERT_TRUE(now_owned == "chat-2 2" || now_owned == "chat-3 2") << now_owned;
    std::cout << "owner before the stop: chat-1 1, after: " << now_owned << "\n";

    // The survivors carry on through the new owner. Each hears its own message back, and so is
    // routed to it: its node gets everything the new owner sequences from then on.
    const auto bob_first = send_until_heard(*bob, "bob after the failover");
    ASSERT_TRUE(bob_first);
    const std::string bob_first_body = last_body_["bob"];
    const auto carol_first = send_until_heard(*carol, "carol after the failover");
    ASSERT_TRUE(carol_first);
    const std::string carol_first_body = last_body_["carol"];
    // Both routed, each hears the other live.
    ASSERT_TRUE(send_until_heard(*bob, "bob once both are routed"));
    ASSERT_TRUE(carol->message(last_body_["bob"]));
    ASSERT_TRUE(send_until_heard(*carol, "carol once both are routed"));
    ASSERT_TRUE(bob->message(last_body_["carol"]));
    // Before that, a node still between owners may have missed the other's first message live
    // (ADR-0035): it is not lost. Each survivor finds both in history under the seqs their
    // senders heard, and any it heard live came under the same seq.
    const std::uint64_t first = std::min(*bob_first, *carol_first);
    for (Client* c : {bob.get(), carol.get()}) {
        const auto page = history(*c, R"(,"after":)" + std::to_string(first - 1));
        ASSERT_TRUE(page) << c->name();
        for (const auto& [body, seq] :
             {std::pair{bob_first_body, *bob_first}, std::pair{carol_first_body, *carol_first}}) {
            EXPECT_TRUE(std::ranges::any_of(
                *page, [&](const Seen& m) { return m.seq == seq && m.body == body; }))
                << c->name() << " cannot find seq " << seq << " in history";
            for (const Seen& m : c->messages()) {
                if (m.body == body) {
                    EXPECT_EQ(m.seq, seq) << c->name();
                }
            }
        }
    }
    const std::string seq_before = last_seq();

    // Resumed, the old owner still believes it owns the room: its write is fenced out.
    stalled.process->signal(SIGCONT);
    const std::string fenced = R"("msg":"fenced out","node":"chat-1","room":")" + room_ +
                               R"(","generation":1,"write":"append"})";
    ASSERT_TRUE(stalled.process->wait_for_output(fenced, seconds(30))) << stalled.process->output();
    const auto refused = alice->wait_for([](const Seen& s) { return s.id == ref(7); });
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->type, "error");
    // "fenced" when the append's own answer comes back first; "unavailable" when the resumed
    // node's heartbeat finds the fence while the append is still out, and its fate is unknown
    // to the node. Either way the client is told it was not sequenced for certain.
    EXPECT_TRUE(refused->reason == "fenced" || refused->reason == "unavailable") << refused->reason;
    EXPECT_EQ(last_seq(), seq_before) << "the stale write updated a row";
    const std::string& logged = stalled.process->output();
    const std::size_t at = logged.find(fenced);
    const std::size_t line = logged.rfind('\n', at) + 1;
    std::cout << "chat-1 logged: " << logged.substr(line, logged.find('\n', at) - line) << "\n";

    // The old owner's node routes to the new owner from here on, and everyone hears it.
    const auto back = send_until_heard(*alice, "alice is back");
    ASSERT_TRUE(back);
    for (Client* c : {bob.get(), carol.get()}) {
        const auto heard = c->message(last_body_["alice"]);
        ASSERT_TRUE(heard) << c->name();
        EXPECT_EQ(heard->seq, *back);
    }
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        EXPECT_FALSE(c->ever_saw("stale")) << c->name() << " was delivered the fenced write";
    }
    EXPECT_NE(ulw::test::http_get(stalled.port, "/metrics").body.find("fenced_writes_total "),
              std::string::npos);
    EXPECT_EQ(ulw::test::http_get(stalled.port, "/metrics").body.find("fenced_writes_total 0\n"),
              std::string::npos);

    // Every node drains cleanly, the resumed one included.
    for (Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (Node& n : nodes_) {
        EXPECT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }

    // Each ran on the reactor it was asked for, not a fallback.
    const std::string reactor = GetParam() == net::ReactorKind::IoUring ? R"("reactor":"io_uring")"
                                                                        : R"("reactor":"epoll")";
    // Bodies, tokens and the node secret never reach a log (section 8.14).
    std::vector<std::string> secrets = tokens_;
    secrets.push_back(node_secret_);
    for (const std::string body :
         {"hello from chat-2", "hello from chat-1", "stale", "bob after the failover",
          "carol after the failover", "bob once both are routed", "carol once both are routed",
          "alice is back"}) {
        secrets.push_back(body);
        secrets.push_back(infra::auth::encode_base64url(body));
    }
    for (const Node& n : nodes_) {
        const std::string& output = n.process->output();
        EXPECT_NE(output.find(reactor + ","), std::string::npos) << n.name << "\n" << output;
        for (const std::string& secret : secrets) {
            EXPECT_EQ(output.find(secret), std::string::npos) << n.name << " logged " << secret;
        }
    }
}

// The build links jemalloc unless a sanitizer is on (ADR-0081); each node says which it got.
TEST_P(ChatClusterTest, EveryNodeAllocatesWithTheAllocatorTheBuildLinked) {
    const std::string expected =
        ULW_EXPECT_JEMALLOC != 0 ? R"("allocator":"jemalloc 5.)" : R"("allocator":"default")";
    for (const Node& n : nodes_) {
        EXPECT_NE(n.process->output().find(expected), std::string::npos) << n.name << ":\n"
                                                                         << n.process->output();
    }
}

TEST_P(ChatClusterTest, ClientsOnEveryNodeSeeEveryMessageInOneOrderByLastSeq) {
    // Two clients on each node, as three users, each user on two nodes.
    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t node = 0; node < nodes_.size(); ++node) {
        for (std::size_t k = 0; k < 2; ++k) {
            clients.push_back(connect(nodes_[node], (node + k) % 3));
            ASSERT_TRUE(clients.back());
            ASSERT_NO_FATAL_FAILURE(join(*clients.back()));
        }
    }
    // Every client sends all its messages at once, without waiting for an answer: the owner
    // sees them interleaved from three nodes.
    constexpr int kEach = 4;
    std::vector<std::string> bodies;
    for (int round = 0; round < kEach; ++round) {
        for (std::size_t i = 0; i < clients.size(); ++i) {
            bodies.push_back(std::format("order check {} from client {}", round, i));
            ASSERT_TRUE(clients[i]->send(
                send_command(room_, bodies.back(), std::format("c{}-{}", i, round))));
        }
    }
    const std::size_t total = bodies.size();
    for (auto& c : clients) {
        ASSERT_TRUE(c->wait_for([&](const Seen&) { return c->messages().size() == total; }))
            << c->name() << " heard " << c->messages().size() << " of " << total;
    }

    const std::vector<Seen> first = clients[0]->messages();
    for (std::size_t k = 0; k < first.size(); ++k) {
        EXPECT_EQ(first[k].seq, first[0].seq + k) << "seqs run without gaps";
    }
    for (auto& c : clients) {
        const std::vector<Seen> heard = c->messages();
        for (std::size_t k = 0; k < total; ++k) {
            EXPECT_EQ(heard[k].seq, first[k].seq) << c->name();
            EXPECT_EQ(heard[k].id, first[k].id) << c->name();
            EXPECT_EQ(heard[k].body, first[k].body) << c->name();
        }
    }
    for (const std::string& body : bodies) {
        EXPECT_EQ(std::ranges::count(first, body, &Seen::body), 1) << body;
    }
    // Each sender's own ack names the seq everyone saw its message under.
    for (std::size_t i = 0; i < clients.size(); ++i) {
        for (int round = 0; round < kEach; ++round) {
            const std::string id = std::format("c{}-{}", i, round);
            const auto ack =
                clients[i]->wait_for([&](const Seen& s) { return s.type == "sent" && s.id == id; });
            ASSERT_TRUE(ack) << id;
            const auto it = std::ranges::find(first, id, &Seen::id);
            ASSERT_NE(it, first.end());
            EXPECT_EQ(ack->seq, it->seq) << id;
        }
    }
    EXPECT_EQ(last_seq(), std::to_string(first.back().seq));
    std::cout << clients.size() << " clients on " << nodes_.size() << " nodes saw seqs "
              << first.front().seq << ".." << first.back().seq
              << " in one order; last_seq in room_state is " << last_seq() << "\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(bodies));
}

TEST_P(ChatClusterTest, ARateLimitedSendIsRefusedAndReachesNobody) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && bob && carol);
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        ASSERT_NO_FATAL_FAILURE(join(*c));
    }
    // Sixteen at once against a burst of ten refilling at two a second: all but ten are
    // refused unless the node took seconds to read them.
    constexpr std::size_t kBurst = 16;
    std::vector<std::string> bodies;
    for (std::size_t i = 0; i < kBurst; ++i) {
        bodies.push_back(std::format("burst message {}", i));
        ASSERT_TRUE(bob->send(send_command(room_, bodies.back(), std::format("b{}", i))));
    }
    std::vector<std::string> limited;
    std::vector<std::string> refused;
    std::uint64_t retry_after_ms = 0;
    for (std::size_t i = 0; i < kBurst; ++i) {
        const std::string id = std::format("b{}", i);
        const auto answer = bob->wait_for(
            [&](const Seen& s) { return (s.type == "sent" || s.type == "error") && s.id == id; });
        ASSERT_TRUE(answer) << id;
        if (answer->type == "error") {
            EXPECT_EQ(answer->reason, "rate_limited") << id;
            ASSERT_TRUE(answer->retry_after_ms) << id;
            retry_after_ms = std::max(retry_after_ms, *answer->retry_after_ms);
            limited.push_back(id);
            refused.push_back(bodies[i]);
        }
    }
    ASSERT_GE(limited.size(), 1U);
    ASSERT_LE(limited.size(), kBurst - 10) << "at most those past the burst";

    // bob tries a marker until one is taken, each try once the last is answered: no clock in
    // the test, only the server's answers. Anything refused would have been sequenced before
    // the marker, on the same connection, so its absence below is final.
    std::size_t marker_refusals = 0;
    for (;;) {
        const std::string id = std::format("marker-{}", marker_refusals);
        ASSERT_TRUE(bob->send(send_command(room_, "after the limit", id)));
        const auto answer = bob->wait_for(
            [&](const Seen& s) { return (s.type == "sent" || s.type == "error") && s.id == id; });
        ASSERT_TRUE(answer) << id;
        if (answer->type == "sent") {
            break;
        }
        ASSERT_EQ(answer->reason, "rate_limited");
        // 2 a second: the bucket refills within half a second, far fewer tries than this.
        ASSERT_LT(++marker_refusals, 100'000U) << "the bucket never refilled";
    }
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        ASSERT_TRUE(c->message("after the limit")) << c->name();
        EXPECT_EQ(c->count([](const Seen& s) {
            return s.type == "message" && s.body.starts_with("burst message");
        }),
                  kBurst - limited.size())
            << c->name();
        for (const std::string& id : limited) {
            EXPECT_EQ(c->count([&](const Seen& s) { return s.type == "message" && s.id == id; }),
                      0U)
                << c->name() << " was delivered " << id;
        }
    }
    EXPECT_EQ(metric(nodes_[1], "messages_rate_limited_total"), limited.size() + marker_refusals);
    std::cout << "bob sent " << kBurst << " at once: " << limited.size()
              << " refused as rate_limited (retry after " << retry_after_ms
              << " ms), delivered to nobody; the rest reached all three nodes\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(bodies, refused));
}

TEST_P(ChatClusterTest, ASendRepeatedWithItsIdIsDeliveredOnceWhereverItIsRepeated) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    const std::string body = "said once, sent three times";
    const auto acks = [&](Client& c) {
        return c.count([](const Seen& s) { return s.type == "sent" && s.id == "retry-1"; });
    };
    ASSERT_TRUE(alice->send(send_command(room_, body, "retry-1")));
    const auto ack =
        alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "retry-1"; });
    ASSERT_TRUE(ack);
    const std::uint64_t seq = ack->seq;
    // Again on the same connection, as after `unavailable`...
    ASSERT_TRUE(alice->send(send_command(room_, body, "retry-1")));
    ASSERT_TRUE(alice->wait_for([&](const Seen&) { return acks(*alice) == 2; }));
    // ...and from a connection on another node, as after a reconnect that landed elsewhere.
    ASSERT_TRUE(bob->message(body));
    auto elsewhere = connect(nodes_[1], 0);
    ASSERT_TRUE(elsewhere);
    ASSERT_NO_FATAL_FAILURE(join(*elsewhere));
    ASSERT_TRUE(elsewhere->send(send_command(room_, body, "retry-1")));
    const auto again =
        elsewhere->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "retry-1"; });
    ASSERT_TRUE(again);
    EXPECT_EQ(again->seq, seq);
    for (const Seen& s : alice->messages()) {
        EXPECT_TRUE(s.id != "retry-1" || s.seq == seq);
    }

    ASSERT_TRUE(alice->send(send_command(room_, "after the retries", "marker")));
    for (Client* c : {alice.get(), bob.get(), elsewhere.get()}) {
        ASSERT_TRUE(c->message("after the retries")) << c->name();
    }
    EXPECT_EQ(alice->count([&](const Seen& s) { return s.body == body; }), 1U);
    EXPECT_EQ(bob->count([&](const Seen& s) { return s.body == body; }), 1U);
    EXPECT_EQ(elsewhere->count([&](const Seen& s) { return s.body == body; }), 0U);
    EXPECT_EQ(last_seq(), std::to_string(seq + 1));
    EXPECT_EQ(metric(nodes_[0], "messages_deduplicated_total") +
                  metric(nodes_[1], "messages_deduplicated_total"),
              2U);
    std::cout << "one message sent three times, twice on chat-1 and once on chat-2: acked as seq "
              << seq << " each time, delivered once\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext({body}));
}

TEST_P(ChatClusterTest, AClientThatComesBackResumesFromItsLastSeq) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    const auto last = send_until_heard(*alice, "before bob left");
    ASSERT_TRUE(last);
    ASSERT_TRUE(bob->message(last_body_["alice"]));
    // bob was chat-2's only client in the room.
    bob.reset();

    std::vector<std::string> missed;
    for (int i = 0; i < 3; ++i) {
        missed.push_back(std::format("while bob was away {}", i));
        ASSERT_TRUE(alice->send(send_command(room_, missed.back(), std::format("away-{}", i))));
        ASSERT_TRUE(alice->message(missed.back()));
    }
    auto back = connect(nodes_[1], 1);
    ASSERT_TRUE(back);
    ASSERT_NO_FATAL_FAILURE(join(*back, last));
    std::vector<std::uint64_t> seqs;
    for (const std::string& body : missed) {
        const auto m = back->message(body);
        ASSERT_TRUE(m) << body;
        seqs.push_back(m->seq);
    }
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{*last + 1, *last + 2, *last + 3}));
    EXPECT_EQ(back->messages().size(), 3U) << "nothing at or before the seq it named";
    ASSERT_TRUE(alice->send(send_command(room_, "welcome back", "live")));
    const auto live = back->message("welcome back");
    ASSERT_TRUE(live);
    EXPECT_EQ(live->seq, *last + 4);
    EXPECT_EQ(metric(nodes_[1], "messages_replayed_total"), 3U);
    std::cout << "bob came back to chat-2 after seq " << *last << " and was sent seqs "
              << seqs.front() << ".." << seqs.back() << " before the live seq " << live->seq
              << "\n";
    missed.emplace_back("welcome back");
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(missed));
}

TEST_P(ChatClusterTest, AClientResumingThroughANodeThatKeptNothingFillsTheGapFromHistory) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    const auto last = send_until_heard(*alice, "before bob left");
    ASSERT_TRUE(last);
    ASSERT_TRUE(bob->message(last_body_["alice"]));
    bob.reset();
    std::vector<std::string> missed;
    for (int i = 0; i < 3; ++i) {
        missed.push_back(std::format("sent while bob was elsewhere {}", i));
        ASSERT_TRUE(alice->send(send_command(room_, missed.back(), std::format("gone-{}", i))));
        ASSERT_TRUE(alice->message(missed.back()));
    }
    // chat-3 was never in the room: it has nothing to send again, only the head to name.
    auto elsewhere = connect(nodes_[2], 1);
    ASSERT_TRUE(elsewhere);
    ASSERT_NO_FATAL_FAILURE(join(*elsewhere, last));
    const auto joined = elsewhere->wait_for([](const Seen& s) { return s.type == "joined"; });
    ASSERT_TRUE(joined);
    EXPECT_EQ(joined->seq, *last + 3) << "the client can tell it missed three messages";
    EXPECT_TRUE(elsewhere->messages().empty());
    // It fills the gap from history, which the store has whichever node the client is on.
    const auto filled = history(*elsewhere, std::format(R"(,"after":{})", *last));
    ASSERT_TRUE(filled);
    ASSERT_EQ(filled->size(), 3U);
    for (std::size_t i = 0; i < filled->size(); ++i) {
        EXPECT_EQ((*filled)[i].seq, *last + 1 + i);
        EXPECT_EQ((*filled)[i].body, missed[i]);
        EXPECT_EQ((*filled)[i].sender, "alice");
    }
    std::cout << "bob came back through chat-3 after seq " << *last
              << ", which kept nothing; joined named the head " << joined->seq
              << ", and history sent seqs " << filled->front().seq << ".." << filled->back().seq
              << "\n";
    missed.push_back(last_body_["alice"]);
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(missed));
}

TEST_P(ChatClusterTest, AReconnectWithinTheGraceIsNoEventAndALeaveIsOneOfflineOnEveryNode) {
    const auto presence = [](std::string_view status) {
        return [status](const Seen& s) {
            return s.type == "presence" && s.user == "alice" && s.status == status;
        };
    };
    const auto is_presence = [](const Seen& s) { return s.type == "presence"; };
    const auto is_presence_after_online = [](const Seen& s) {
        return s.type == "presence" && s.status != "online";
    };
    // bob watches alice from every node before she connects.
    std::vector<std::unique_ptr<Client>> watchers;
    for (const Node& n : nodes_) {
        watchers.push_back(connect(n, 1));
        ASSERT_TRUE(watchers.back());
        ASSERT_TRUE(watchers.back()->send(R"({"type":"watch","user":"alice"})"));
        const auto answer = watchers.back()->wait_for(
            [](const Seen& s) { return s.type == "watching" || s.type == "error"; });
        ASSERT_TRUE(answer) << n.name;
        EXPECT_EQ(answer->type, "watching") << answer->reason;
        EXPECT_EQ(answer->status, "offline");
    }
    auto alice = connect(nodes_[0], 0);
    ASSERT_TRUE(alice);
    for (auto& w : watchers) {
        ASSERT_TRUE(w->wait_for(presence("online")));
    }

    // alice drops off chat-1 and is back through chat-2 within the grace. chat-1 answers
    // chat-2's announcement with an ack (bob watches there too) and, once its grace has run
    // out, says she is offline there: two events, which change nothing for anyone watching.
    const std::uint64_t before = metric(nodes_[0], "presence_events_sent_total");
    alice.reset();
    alice = connect(nodes_[1], 0);
    ASSERT_TRUE(alice);
    // Each check is a request: paced, as readiness is.
    ASSERT_TRUE(nodes_[0].process->poll_until(
        [&] { return metric(nodes_[0], "presence_events_sent_total") >= before + 2; }, seconds(30),
        kReadyCheckPeriod));
    // Everything chat-1 sent has had a grace's time to reach every node.
    const auto quiet_until = [](std::chrono::steady_clock::time_point until) {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                            until - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds{1});
    };
    auto until = std::chrono::steady_clock::now() + kGrace;
    for (std::size_t i = 0; i < watchers.size(); ++i) {
        EXPECT_FALSE(watchers[i]->wait_for(is_presence_after_online, quiet_until(until)))
            << nodes_[i].name << " heard the reconnect";
        EXPECT_EQ(watchers[i]->count(is_presence), 1U) << "online, and nothing since";
    }

    // Now she leaves for good.
    alice.reset();
    for (std::size_t i = 0; i < watchers.size(); ++i) {
        ASSERT_TRUE(watchers[i]->wait_for(presence("offline"))) << nodes_[i].name;
    }
    // Twice the grace more: long enough for a second offline from any node to arrive.
    until = std::chrono::steady_clock::now() + (kGrace * 2);
    for (std::size_t i = 0; i < watchers.size(); ++i) {
        EXPECT_FALSE(watchers[i]->wait_for([](const Seen&) { return false; }, quiet_until(until)));
        EXPECT_EQ(watchers[i]->count(presence("offline")), 1U) << nodes_[i].name;
        EXPECT_EQ(watchers[i]->count(is_presence), 2U) << nodes_[i].name;
    }
    // Sequenced like any room's, and kept nowhere.
    auto conn = db_->session();
    const std::string sequenced = ulw::test::scalar(
        conn, "SELECT coalesce(sum(last_seq), 0) FROM room_state WHERE kind = 'presence'",
        Params{});
    EXPECT_NE(sequenced, "0");
    EXPECT_EQ(
        ulw::test::scalar(conn,
                          "SELECT count(*) FROM chat_messages JOIN room_state USING (room_id) "
                          "WHERE kind = 'presence'",
                          Params{}),
        "0");
    std::cout << "presence rooms took " << sequenced << " seqs and stored no row\n";
    std::cout << "alice reconnected through chat-2 within the " << kGrace.count()
              << " ms grace: no event on any node; she left: one offline on each of "
              << nodes_.size() << " nodes\n";
}

TEST_P(ChatClusterTest, AUserNobodyWatchesCostsNoPresenceMessage) {
    const auto total = [&](std::string_view name) {
        std::uint64_t sum = 0;
        for (const Node& n : nodes_) {
            sum += metric(n, name);
        }
        return sum;
    };
    const std::uint64_t sent = total("presence_events_sent_total");
    const std::uint64_t received = total("presence_events_received_total");
    const std::uint64_t forwards = total("forwards_total");
    auto dave = connect(nodes_[0], 3);
    ASSERT_TRUE(dave);
    EXPECT_EQ(metric(nodes_[0], "presence_rooms"), 1U);
    dave.reset();
    // The grace runs out, and chat-1 leaves dave's room: nothing is left of him.
    ASSERT_TRUE(nodes_[0].process->poll_until(
        [&] { return metric(nodes_[0], "presence_rooms") == 0; }, seconds(30), kReadyCheckPeriod));
    EXPECT_EQ(total("presence_events_sent_total"), sent);
    EXPECT_EQ(total("presence_events_received_total"), received);
    EXPECT_EQ(total("forwards_total"), forwards);
    std::cout << "dave came and went with nobody watching: presence events sent "
              << total("presence_events_sent_total") - sent << ", received "
              << total("presence_events_received_total") - received << ", forwards "
              << total("forwards_total") - forwards << " across " << nodes_.size() << " nodes\n";
}

TEST_P(ChatClusterTest, HistorySurvivesARestartOfEveryNodeInTheOrderItWasSent) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    std::vector<std::string> sent;
    for (int i = 0; i < 6; ++i) {
        Client& from = i % 2 == 0 ? *alice : *bob;
        sent.push_back(std::format("kept across restarts {}", i));
        ASSERT_TRUE(from.send(send_command(room_, sent.back(), std::format("keep-{}", i))));
        ASSERT_TRUE(alice->message(sent.back()));
    }
    const std::vector<Seen> live = alice->messages();
    alice.reset();
    bob.reset();

    for (Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (Node& n : nodes_) {
        ASSERT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }
    for (Node& n : nodes_) {
        ASSERT_NO_FATAL_FAILURE(start(n, jwks_));
    }
    ASSERT_NO_FATAL_FAILURE(wait_ready());

    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(carol);
    ASSERT_NO_FATAL_FAILURE(join(*carol));
    const auto newest = history(*carol, R"(,"limit":4)");
    ASSERT_TRUE(newest);
    const auto oldest = history(*carol, std::format(R"(,"before":{})", newest->back().seq));
    ASSERT_TRUE(oldest);
    std::vector<Seen> all = *newest;
    all.insert(all.end(), oldest->begin(), oldest->end());
    ASSERT_EQ(all.size(), live.size());
    // Newest first from the store, as the live order reversed, byte for byte.
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Seen& was = live[live.size() - 1 - i];
        EXPECT_EQ(all[i].seq, was.seq);
        EXPECT_EQ(all[i].id, was.id);
        EXPECT_EQ(all[i].sender, was.sender);
        EXPECT_EQ(all[i].body, was.body);
    }
    const auto past_the_start = history(*carol, std::format(R"(,"before":{})", live.front().seq));
    ASSERT_TRUE(past_the_start);
    EXPECT_TRUE(past_the_start->empty());
    std::cout << "after all three nodes restarted, history gave seqs " << all.front().seq << ".."
              << all.back().seq << " newest first, as they were delivered\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(sent));
}

TEST_P(ChatClusterTest, AResendAfterEveryNodeRestartedIsKnownToTheStoreAlone) {
    auto alice = connect(nodes_[0], 0);
    ASSERT_TRUE(alice);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    const std::string body = "sent before the restart";
    ASSERT_TRUE(alice->send(send_command(room_, body, "kept-key")));
    const auto ack =
        alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "kept-key"; });
    ASSERT_TRUE(ack);
    const std::string owned_before = owner();
    alice.reset();
    // Every node's memory of recent keys goes with it, and the room gets a new owner.
    for (Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (Node& n : nodes_) {
        ASSERT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }
    for (Node& n : nodes_) {
        ASSERT_NO_FATAL_FAILURE(start(n, jwks_));
    }
    ASSERT_NO_FATAL_FAILURE(wait_ready());

    // The resends go to the room's owner, which answers them itself. A send forwarded from
    // another node is answered `unavailable` when the forward times out (kForwardTimeout, while
    // the node link is still coming up, say), yet may still reach the owner, which then
    // remembers the key: the resend would be answered from that memory, not the store. The
    // join through chat-2 resolved the room, so the owner since the restart is recorded by now.
    auto again = connect(nodes_[1], 0);
    ASSERT_TRUE(again);
    ASSERT_NO_FATAL_FAILURE(join(*again));
    const std::string owned = owner();
    const auto generation = [](std::string_view assignment) {
        return core::parse_integer<std::uint64_t>(assignment.substr(assignment.find(' ') + 1))
            .value_or(0);
    };
    ASSERT_GT(generation(owned), generation(owned_before))
        << "no claim since the restart: " << owned_before << ", then " << owned;
    const auto owner_node =
        std::ranges::find(nodes_, owned.substr(0, owned.find(' ')), &Node::name);
    ASSERT_NE(owner_node, nodes_.end()) << owned;
    if (owner_node != nodes_.begin() + 1) {
        again = connect(*owner_node, 0);
        ASSERT_TRUE(again);
        ASSERT_NO_FATAL_FAILURE(join(*again));
    }
    // Even a repeat is a commit in the store, which may take past the store's timeout under
    // load: the node then answers `unavailable`, and the client sends again, as any would.
    const auto repeat = send_until_answered(*again, body, "kept-key");
    ASSERT_TRUE(repeat) << "the resend was not answered" << what_was_seen(*again);
    ASSERT_EQ(repeat->type, "sent") << repeat->reason << what_was_seen(*again);
    EXPECT_EQ(repeat->seq, ack->seq);
    const auto conflict = send_until_answered(*again, "another body", "kept-key");
    ASSERT_TRUE(conflict) << "another body was not answered" << what_was_seen(*again);
    ASSERT_EQ(conflict->type, "error") << what_was_seen(*again);
    EXPECT_EQ(conflict->reason, "conflict") << what_was_seen(*again);
    const auto marker = send_until_answered(*again, "after the resends", "marker");
    ASSERT_TRUE(marker) << "the marker was not answered" << what_was_seen(*again);
    ASSERT_EQ(marker->type, "sent") << marker->reason << what_was_seen(*again);
    ASSERT_TRUE(again->message("after the resends")) << what_was_seen(*again);
    // Never sequenced again: the marker is the next seq, and nothing arrived under the key
    // but the first message, under its own seq.
    EXPECT_EQ(last_seq(), std::to_string(ack->seq + 1));
    for (const Seen& s : again->messages()) {
        EXPECT_TRUE(s.id != "kept-key" || (s.seq == ack->seq && s.body == body));
    }
    EXPECT_FALSE(again->ever_saw("another body"));
    for (const Node& n : nodes_) {
        EXPECT_EQ(metric(n, "messages_deduplicated_total"), 0U) << n.name << " answered it";
    }
    std::cout << "after every node restarted, the resend got seq " << repeat->seq
              << " from the store through its owner " << owned
              << ", and another body under its id was a conflict; "
              << again->count(
                     [](const Seen& s) { return s.type == "error" && s.reason == "unavailable"; })
              << " sends were answered unavailable and sent again\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext({body, "another body"}, {"another body"}));
}

TEST_P(ChatClusterTest, ARoomWithMembersRefusesEveryoneElse) {
    const std::string members_only = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(members_only, {"alice", "bob"}));
    auto alice = connect(nodes_[0], 0);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && carol);
    EXPECT_EQ(join_answer(*alice, members_only), "joined");
    EXPECT_EQ(join_answer(*carol, members_only), "not_member");
    ASSERT_TRUE(carol->send(send_command(members_only, "let me in", "sneak")));
    const auto sneak = carol->wait_for([](const Seen& s) { return s.id == "sneak"; });
    ASSERT_TRUE(sneak);
    EXPECT_EQ(sneak->reason, "not_joined");
    ASSERT_TRUE(alice->send(send_command(members_only, "members only", "inside")));
    ASSERT_TRUE(alice->message("members only"));
    EXPECT_FALSE(carol->ever_saw("members only"));
}

// Members are taken off a list in the database, by the product or an operator. Every node hears
// of it and stops delivering the room to that user's sockets at once (ADR-0073), wherever the
// room is owned.
TEST_P(ChatClusterTest, AMemberDeletedInTheDatabaseStopsReceivingOnEveryNode) {
    const std::string members_only = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(members_only, {"alice", "bob", "carol"}));
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[1], 2);
    ASSERT_TRUE(alice && bob && carol);
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        ASSERT_EQ(join_answer(*c, members_only), "joined");
    }
    ASSERT_TRUE(alice->send(send_command(members_only, "before", "m1")));
    ASSERT_TRUE(bob->message("before"));

    auto conn = db_->session();
    ASSERT_TRUE(conn.exec("DELETE FROM chat_members WHERE room_id = $1::text::uuid AND user_id = "
                          "'bob'",
                          Params{}.add_text(members_only)));
    const auto removed = bob->wait_for([&](const Seen& s) {
        return s.type == "error" && s.room == members_only && s.reason == "not_member";
    });
    ASSERT_TRUE(removed) << "bob was never told";

    ASSERT_TRUE(alice->send(send_command(members_only, "after", "m2")));
    // Carol shares bob's node, so its fan-out of "after" reached both sockets together; bob's
    // answer to a command sent once carol has it comes behind anything that fan-out sent him.
    ASSERT_TRUE(carol->message("after"));
    ASSERT_TRUE(bob->send(send_command(members_only, "let me back", "m3")));
    const auto refused = bob->wait_for([](const Seen& s) { return s.id == "m3"; });
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->reason, "not_joined");
    EXPECT_FALSE(bob->ever_saw("after"));
}

TEST_P(ChatClusterTest, AGroupRoomWithNoMembersRefusesEveryoneAndCannotBeOpenedLater) {
    const std::string nobody = core::RoomId::generate(clock_, random_).to_string();
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    EXPECT_EQ(join_answer(*alice, nobody), "not_member");
    // Its first join recorded it as a group chat. A room id cannot ask to be a live chat (only a
    // stream's room is one, ADR-0070), and the database refuses to record it as one.
    EXPECT_EQ(join_answer(*bob, nobody, R"(,"kind":"live")"), "malformed");
    auto conn = db_->session();
    EXPECT_FALSE(
        conn.exec("UPDATE chat_rooms SET kind = 'stream_live_chat' WHERE room_id = $1::text::uuid",
                  Params{}.add_text(nobody)));
    EXPECT_EQ(ulw::test::scalar(conn, "SELECT kind FROM chat_rooms WHERE room_id = $1::text::uuid",
                                Params{}.add_text(nobody)),
              "group_chat");
}

// Every join of a room nobody recorded is refused and records the room (ADR-0054); past a
// user's allowance, 20 at once, the joins are refused the same and record nothing, so one user
// cannot fill the table.
TEST_P(ChatClusterTest, RefusedJoinsRecordAtMostTheUsersAllowanceOfRooms) {
    constexpr std::size_t kAllowance = 20;
    constexpr std::size_t kJoins = kAllowance + 5;
    auto alice = connect(nodes_[0], 0);
    ASSERT_TRUE(alice);
    std::vector<std::string> rooms;
    for (std::size_t i = 0; i < kJoins; ++i) {
        rooms.push_back(core::RoomId::generate(clock_, random_).to_string());
        ASSERT_TRUE(alice->send(R"({"type":"join","room":")" + rooms.back() + R"("})"));
        const auto answer = alice->wait_for([&](const Seen& s) {
            return s.type == "error" && s.room == rooms.back() && s.reason != "not_joined";
        });
        ASSERT_TRUE(answer) << i;
        EXPECT_EQ(answer->reason, "not_member") << i;
    }
    auto conn = db_->session();
    std::string list = "{";
    for (const std::string& room : rooms) {
        list += (list.size() > 1 ? "," : "") + room;
    }
    list += "}";
    EXPECT_EQ(ulw::test::scalar(conn,
                                "SELECT count(*) FROM chat_rooms WHERE room_id = ANY($1::uuid[])",
                                Params{}.add_text(list)),
              std::to_string(kAllowance));
}

TEST_P(ChatClusterTest, AStreamsChatRefusesViewersUntilTheServerOpensItAndRecordsNothing) {
    const std::string stream = "unopened-" + room_.substr(0, 8);
    auto alice = connect(nodes_[0], 0);
    ASSERT_TRUE(alice);
    EXPECT_EQ(stream_join_answer(*alice, stream), "not_live");
    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(
                  conn, "SELECT count(*) FROM chat_rooms WHERE room_id = live_chat_room($1)",
                  Params{}.add_text(stream)),
              "0");
}

TEST_P(ChatClusterTest, AnOpenedStreamsChatAdmitsAnyoneByTheStreamsName) {
    const std::string stream = "open-" + room_.substr(0, 8);
    ASSERT_EQ(record_live(stream), "stream_live_chat");
    auto alice = connect(nodes_[0], 0);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && carol);
    EXPECT_EQ(stream_join_answer(*alice, stream), "joined");
    EXPECT_EQ(stream_join_answer(*carol, stream), "joined");
    const auto joined = alice->wait_for([](const Seen& s) { return s.type == "joined"; });
    ASSERT_TRUE(joined);
    const std::string live = joined->room;
    // Named by its id, it is not joined: a stream's room is reached only by the stream.
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(bob);
    EXPECT_EQ(join_answer(*bob, live), "bad_room");
    ASSERT_TRUE(carol->send(send_command(live, "hello, stream", "live-1")));
    ASSERT_TRUE(alice->message("hello, stream"));
}

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#elif defined(__has_feature)
constexpr bool kAddressSanitizer = __has_feature(address_sanitizer);
#else
constexpr bool kAddressSanitizer = false;
#endif

// A crash would otherwise write the database password, the node secret and live bearer tokens
// to disk.
TEST_P(ChatClusterTest, ANodeCanWriteNoCoreFile) {
    const ulw::test::RaisedCoreLimit limit;
    if (!limit.raised()) {
        GTEST_SKIP() << "the hard core limit is 0 here; there is nothing to lower";
    }
    Node& node = nodes_[0];
    node.process->signal(SIGTERM);
    ASSERT_EQ(node.process->wait_exit(seconds(30)), 0) << node.process->output();
    ASSERT_NO_FATAL_FAILURE(start(node, jwks_));
    EXPECT_EQ(ulw::test::core_limit_of(node.process->pid()), std::optional<std::string>("0 0"));
}

// Resident memory, from /proc, the kernel's socket buffers aside. What the node allocates is
// anonymous; pages of its binary and libraries (file) come in as code first runs and hold
// nothing for a client.
struct Resident {
    std::uint64_t anon_kib = 0;
    std::uint64_t file_kib = 0;
};

Resident resident(pid_t pid) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    Resident r;
    const auto kib = [](std::string_view rest) {
        const std::size_t digits = rest.find_first_of("0123456789");
        const std::size_t end = rest.find(' ', digits);
        return core::parse_integer<std::uint64_t>(rest.substr(digits, end - digits)).value_or(0);
    };
    while (std::getline(status, line)) {
        if (line.starts_with("RssAnon:")) {
            r.anon_kib = kib(std::string_view(line).substr(8));
        } else if (line.starts_with("RssFile:")) {
            r.file_kib = kib(std::string_view(line).substr(8));
        }
    }
    return r;
}

TEST_P(ChatClusterTest, SlowViewersCostTheirNodeNoMemoryAndOthersMissNothing) {
    const std::string stream = "m32-" + room_.substr(0, 8);
    ASSERT_EQ(record_live(stream), "stream_live_chat");
    const auto join_live = [&](Client& c) {
        EXPECT_TRUE(c.send(R"({"type":"join","stream":")" + stream + R"("})"));
        const auto joined =
            c.wait_for([](const Seen& s) { return s.type == "joined" || s.type == "error"; });
        EXPECT_TRUE(joined && joined->type == "joined") << c.name();
        return joined ? joined->room : std::string();
    };
    // Ten senders on each node, each a viewer too, and two viewers that only read. Eight slow
    // viewers are on chat-2, each with its receive buffer fixed at 64 KiB (128 KiB in the
    // kernel). Every 90 messages sequenced, once chat-2's lossy_drops_total shows they are
    // behind, each empties its receive buffer, at most half of what those messages send it
    // (246 KiB), and takes in what it read, answering its node's pings as any reader does.
    // None stops outright: its node closes a connection that acknowledges nothing for 20 s
    // (stall_timeout), or from which nothing has come for 75 s (idle_timeout), which this test
    // can outlast on a loaded machine. A read that empties the buffer reopens the window at once,
    // so each acknowledges some at every read. One of part of a full buffer may not (the receiver's
    // silly window avoidance): reading 64 KiB, half the buffer, the window opened only every other
    // read, 180 messages apart, which took up to 14 s under ASan on a developer's machine and past
    // 20 s on CI runners, where the node rightly closed such viewers as stalled. The kernel's own
    // count of a shut window (TCP_USER_TIMEOUT) ended them too, and is off on client connections
    // (ADR-0070).
    std::vector<std::unique_ptr<Client>> senders;
    std::vector<std::unique_ptr<Client>> viewers;
    std::string live;
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        for (int k = 0; k < 10; ++k) {
            senders.push_back(connect_as(nodes_[n], std::format("sender-{}-{}", n, k)));
            ASSERT_TRUE(senders.back());
            live = join_live(*senders.back());
        }
        for (int k = 0; k < 2; ++k) {
            viewers.push_back(connect_as(nodes_[n], std::format("viewer-{}-{}", n, k)));
            ASSERT_TRUE(viewers.back());
            join_live(*viewers.back());
        }
    }
    constexpr int kSlowReceiveBuffer = 64 * 1024;
    constexpr std::size_t kReadEvery = 90;
    // More than the kernel ever holds for a 128 KiB buffer: one read empties it.
    constexpr std::size_t kSlowRead = std::size_t{256} * 1024;
    Node& slow_node = nodes_[1];
    std::vector<std::unique_ptr<Client>> slow;
    for (int k = 0; k < 8; ++k) {
        slow.push_back(connect_as(slow_node, std::format("slow-viewer-{}", k), kSlowReceiveBuffer));
        ASSERT_TRUE(slow.back());
        ASSERT_EQ(join_live(*slow.back()), live);
    }
    ASSERT_FALSE(HasFailure());

    // 900 messages of 2000 bytes, about 2.6 MiB for each viewer: rounds of one message per
    // sender, each round read by everyone but the slow viewers before the next, so that no
    // one else is ever behind. A send the room or the user turns away as rate_limited is tried
    // again, with a new id since it was never sequenced, once the refusal's retry_after_ms has
    // passed, and first on its node; the room's allowance (40, then 20 a second on each node)
    // sets the pace.
    constexpr std::size_t kMessages = 900;
    // Past the first half, the room keeps its most and the slow viewers are long behind.
    constexpr std::size_t kSettled = 450;
    const auto body = [](std::size_t k) {
        std::string b = std::format("live {} ", k);
        b.resize(2'000, '.');
        return b;
    };
    std::vector<std::optional<std::size_t>> holding(senders.size());
    std::vector<std::string> ids(senders.size());
    std::size_t next = 0;
    std::size_t acked = 0;
    std::size_t attempts = 0;
    std::size_t reads = 0;
    std::uint64_t drops_at_read = 0;
    std::vector<std::size_t> read_bytes(slow.size());
    std::uint64_t head = 0;
    std::uint64_t settled_seq = 0;
    std::size_t sampled = 0;
    std::vector<Resident> samples;
    // Where each of the others' last wait for the head found it. Every wait looks only at what
    // is new: going through all a client had heard for each frame it read made this process,
    // not the nodes, set the pace, until under ASan on a loaded runner 90 messages took longer
    // than stall_timeout and the slow viewers were rightly closed between two of their reads.
    std::map<const Client*, std::size_t> found;
    for (auto* group : {&senders, &viewers}) {
        for (auto& c : *group) {
            found[c.get()] = 0;
        }
    }
    // A node that turned a send away is not sent to again until the refusal's retry_after_ms has
    // passed: the allowance it ran out of has a token then. Its refused sender goes first, so
    // that no other sender on the node takes that token before it. A send has two allowances,
    // its user's and then the room's, and a refusal names the wait for the one that ran out, so
    // the other may refuse the retry once; a third refusal in a row means a limit did not lift
    // when the node said it would.
    using Clock = std::chrono::steady_clock;
    std::array<Clock::time_point, 3> not_before{};
    std::array<std::size_t, 3> first_on{};
    std::array<bool, 3> waited{};
    std::array<int, 3> refused_after_wait{};
    while (acked < kMessages) {
        std::array<bool, 3> refused{};
        for (std::size_t turn = 0; turn < senders.size(); ++turn) {
            const std::size_t node = turn / 10;
            const std::size_t s = (node * 10) + ((first_on.at(node) + turn) % 10);
            if (refused.at(node) || Clock::now() < not_before.at(node)) {
                continue;
            }
            if (!holding[s] && next < kMessages) {
                holding[s] = next++;
            }
            if (!holding[s]) {
                continue;
            }
            // unavailable leaves the send's fate unknown: it goes again under the same id, and
            // is sequenced once (ADR-0043). A send turned away was never sequenced.
            if (ids[s].empty()) {
                ids[s] = std::format("m{}-{}", *holding[s], attempts);
            }
            ++attempts;
            // The answer is the first for this id after the send: an id sent again after
            // unavailable has its earlier answer before it.
            const std::size_t before = senders[s]->seen().size();
            ASSERT_TRUE(senders[s]->send(send_command(live, body(*holding[s]), ids[s])));
            const auto at = senders[s]->wait_from(before, [&](const Seen& seen) {
                return (seen.type == "sent" || seen.type == "error") && seen.id == ids[s];
            });
            ASSERT_TRUE(at) << ids[s];
            const Seen answer = senders[s]->seen()[*at];
            const bool after_wait = std::exchange(waited.at(node), false);
            if (answer.type == "sent") {
                refused_after_wait.at(node) = 0;
                head = std::max(head, answer.seq);
                holding[s].reset();
                ids[s].clear();
                ++acked;
            } else if (answer.reason == "rate_limited") {
                refused_after_wait.at(node) = after_wait ? refused_after_wait.at(node) + 1 : 0;
                ASSERT_LT(refused_after_wait.at(node), 2)
                    << senders[s]->name() << " was turned away again after waiting out both "
                    << "allowances' retry_after_ms: the rate limit never lifted";
                ASSERT_TRUE(answer.retry_after_ms) << ids[s];
                refused.at(node) = true;
                waited.at(node) = true;
                first_on.at(node) = s % 10;
                not_before.at(node) =
                    Clock::now() + std::chrono::milliseconds(*answer.retry_after_ms);
                ids[s].clear();
            } else {
                ASSERT_EQ(answer.reason, "unavailable") << ids[s];
            }
        }
        // When every node with something to send is waiting out a refusal, the soonest of them
        // is waited for on its refused sender's connection, taking in what arrives meanwhile.
        const auto pending = [&](std::size_t n) {
            return next < kMessages ||
                   std::ranges::any_of(std::span(holding).subspan(n * 10, 10),
                                       [](const auto& h) { return h.has_value(); });
        };
        const bool any_sendable =
            std::ranges::any_of(std::array<std::size_t, 3>{0, 1, 2}, [&](std::size_t n) {
                return pending(n) && Clock::now() >= not_before.at(n);
            });
        if (!any_sendable && acked < kMessages) {
            std::size_t n = 3;
            for (std::size_t k = 0; k < 3; ++k) {
                if (pending(k) && (n == 3 || not_before.at(k) < not_before.at(n))) {
                    n = k;
                }
            }
            ASSERT_LT(n, 3U);
            Client& refused_sender = *senders[(n * 10) + first_on.at(n)];
            const auto left =
                std::chrono::ceil<std::chrono::milliseconds>(not_before.at(n) - Clock::now());
            if (left.count() > 0) {
                refused_sender.wait_from(
                    refused_sender.seen().size(), [](const Seen&) { return false; }, left);
                ASSERT_EQ(refused_sender.ending(), "open") << refused_sender.name();
            }
        }
        for (auto* group : {&senders, &viewers}) {
            for (auto& c : *group) {
                // What came before the last round's head came before this one's too.
                std::size_t& from = found.at(c.get());
                const auto at = c->wait_from(from, [&](const Seen& seen) {
                    return seen.type == "message" && seen.seq >= head;
                });
                ASSERT_TRUE(at) << c->name() << " never got seq " << head;
                from = *at;
            }
        }
        if (acked / kReadEvery > reads) {
            reads = acked / kReadEvery;
            if (const std::uint64_t drops = metric(slow_node, "lossy_drops_total");
                drops > drops_at_read) {
                drops_at_read = drops;
                for (std::size_t k = 0; k < slow.size(); ++k) {
                    read_bytes[k] += slow[k]->trickle(kSlowRead);
                }
            }
        }
        if (acked >= kSettled && acked / kReadEvery > sampled) {
            sampled = acked / kReadEvery;
            samples.push_back(resident(slow_node.process->pid()));
            if (samples.size() == 1) {
                settled_seq = head;
            }
        }
    }
    samples.push_back(resident(slow_node.process->pid()));
    // Slow, not stopped: none was closed as stalled or idle while it trickled.
    for (auto& c : slow) {
        EXPECT_EQ(c->ending(), "open") << c->name();
    }
    EXPECT_EQ(metric(slow_node, "stalled_readers_total"), 0U);

    // Everyone who kept reading got every message, once, in the room's order.
    for (auto* group : {&senders, &viewers}) {
        for (auto& c : *group) {
            const std::vector<Seen> heard = c->messages();
            ASSERT_EQ(heard.size(), kMessages) << c->name();
            for (std::size_t k = 0; k < heard.size(); ++k) {
                ASSERT_EQ(heard[k].seq, k + 1) << c->name();
            }
        }
    }
    // What the slow viewers' node held for them did not grow with what they were sent. Past
    // the settled point each can be queued at most lossy_backlog and one message (67 KiB); the
    // room's kept messages (256 KiB) were full long before; each message sequenced adds one
    // remembered key (ADR-0043), about 300 bytes; and the allocator gets 1 MiB of its own.
    // Nothing there grows with how far behind a viewer is, where an unbounded queue would hold
    // each one's 1.3 MiB more.
    //
    // That bound holds for the growth from the settled point to the end without its largest
    // stretch of 90 messages, which is held apart to a bound of its own: the most the node can
    // take on at once without holding anything more for anyone.
    // - Send queues take 16 KiB chunks from a pool that never gives them back (send_queue.hpp),
    //   so the pool steps up to a new high-water mark whenever more of the node's queues are
    //   full at once than ever before. Each of its 20 connections (10 senders, 2 viewers and
    //   8 slow viewers) is lossy and queues at most lossy_backlog and one message, 68523 bytes,
    //   which spans at most 6 chunks when its first is part read: 20 x 96 KiB = 1920 KiB.
    //   Its 4 node-channel connections (it dials the other two nodes and each dials it) hold
    //   at most kMaxPeerBacklog (16 frames of 65792 bytes) and the frame sent before that is
    //   checked, 1118464 bytes, at most 70 chunks: 4 x 1120 KiB = 4480 KiB.
    // - glibc gives a thread that finds its arena locked a new one, and that thread's working
    //   set is allocated there again while the old arena keeps what was freed in it: the room's
    //   kept messages (256 KiB) and the arena's top pad (M_TOP_PAD, 128 KiB).
    // Every allocation here is far below the mmap threshold (128 KiB, rising only after a larger
    // mmapped block is freed), so no trim threshold is raised to leave more behind.
    // A leak gains nothing from the split: it grows every stretch, and all but one count
    // against the same bound as the whole did.
    //
    // Only anonymous memory counts; file pages are reported. AddressSanitizer keeps freed
    // memory in its quarantine instead of returning it, so its resident memory grows with every
    // free, bounded or not: under it the numbers are only reported.
    ASSERT_GE(samples.size(), 3U);
    const std::uint64_t allowed_kib =
        (slow.size() * 67) + ((kMessages - settled_seq) * 300 / 1024) + 1024;
    constexpr std::uint64_t kClients = 20;
    constexpr std::uint64_t kClientQueueKib = std::uint64_t{6} * 16;
    constexpr std::uint64_t kPeers = 4;
    constexpr std::uint64_t kPeerQueueKib = std::uint64_t{70} * 16;
    constexpr std::uint64_t kKeptKib = 256;
    constexpr std::uint64_t kTopPadKib = 128;
    const std::uint64_t step_kib =
        (kClients * kClientQueueKib) + (kPeers * kPeerQueueKib) + kKeptKib + kTopPadKib;
    const std::uint64_t owed_kib = slow.size() * (kMessages - settled_seq) * 2'700 / 1024;
    const auto grew = [](std::uint64_t from, std::uint64_t to) { return to - std::min(to, from); };
    std::uint64_t largest_kib = 0;
    std::string stretches;
    for (std::size_t k = 1; k < samples.size(); ++k) {
        const std::uint64_t anon = grew(samples[k - 1].anon_kib, samples[k].anon_kib);
        largest_kib = std::max(largest_kib, anon);
        stretches +=
            std::format(" {}/{}", anon, grew(samples[k - 1].file_kib, samples[k].file_kib));
    }
    const Resident& settled = samples.front();
    const Resident& last = samples.back();
    const std::uint64_t total_kib = grew(settled.anon_kib, last.anon_kib);
    const std::uint64_t rest_kib = total_kib - std::min(total_kib, largest_kib);
    const std::string memory = std::format(
        "anonymous memory {} KiB at seq {}, {} KiB at seq {} (file {} to {} KiB); growth per 90 "
        "messages, anonymous/file KiB:{}",
        settled.anon_kib, settled_seq, last.anon_kib, kMessages, settled.file_kib, last.file_kib,
        stretches);
    EXPECT_TRUE(kAddressSanitizer || rest_kib < allowed_kib)
        << memory << "; without its largest stretch it grew " << rest_kib << " KiB, allowed "
        << allowed_kib;
    EXPECT_TRUE(kAddressSanitizer || largest_kib < step_kib)
        << memory << "; its largest stretch grew " << largest_kib << " KiB, allowed " << step_kib;

    // Reading in full, each slow viewer gets the rest of what it was owed, in order, up to the
    // last message, after gaps where it dropped the rest, which its node counted exactly.
    std::uint64_t missed = 0;
    std::size_t fewest = kMessages;
    for (auto& c : slow) {
        ASSERT_TRUE(c->wait_for([&](const Seen& seen) {
            return seen.type == "message" && seen.seq == kMessages;
        })) << c->name()
            << " stopped after " << c->messages().size() << " messages, at seq "
            << (c->messages().empty() ? 0 : c->messages().back().seq) << "; its connection "
            << c->ending() << "; its node closed " << metric(slow_node, "stalled_readers_total")
            << " as stalled and " << metric(slow_node, "slow_consumers_total")
            << " as slow consumers";
        const std::vector<Seen> got = c->messages();
        for (std::size_t k = 1; k < got.size(); ++k) {
            ASSERT_LT(got[k - 1].seq, got[k].seq) << c->name();
        }
        // A message is at least its body in base64url (2667 bytes) on the wire. A slow viewer was
        // sent only what it read, what its node's send buffer (64 KiB), its own receive buffer
        // (128 KiB) and its queue before it counts as behind (lossy_backlog, 64 KiB, and one
        // message of at most 2987 bytes) held at the end, and the newest 64 it was owed then.
        const auto k = static_cast<std::size_t>(&c - slow.data());
        const std::size_t bound =
            ((read_bytes[k] + (std::size_t{256} * 1024) + 2'987) / 2'667) + 64;
        EXPECT_LE(got.size(), bound) << c->name() << " read " << read_bytes[k] << " bytes";
        missed += kMessages - got.size();
        fewest = std::min(fewest, got.size());
    }
    EXPECT_GT(missed, 0U);
    EXPECT_EQ(metric(slow_node, "lossy_drops_total"), missed);
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        if (n != 1) {
            EXPECT_EQ(metric(nodes_[n], "lossy_drops_total"), 0U) << nodes_[n].name;
        }
    }
    std::cout << senders.size() + viewers.size() << " viewers on " << nodes_.size()
              << " nodes got all " << kMessages << " messages in order; " << slow.size()
              << " slow ones got as few as " << fewest << ", and their node dropped " << missed
              << " for them; " << memory << "; without the largest stretch (" << largest_kib
              << " KiB, allowed " << step_kib << ") it grew " << rest_kib << " KiB, allowed "
              << allowed_kib << ", having sent them " << owed_kib << " KiB more; "
              << attempts - kMessages << " sends were turned away or unanswered and tried again\n";
    for (std::size_t k = 0; k < slow.size(); ++k) {
        std::cout << slow[k]->name() << " read " << read_bytes[k]
                  << " bytes while it trickled and got " << slow[k]->messages().size() << " of "
                  << kMessages << " messages\n";
    }
}

// No database is reached: the connection string is refused before any connection is tried.
TEST(ChatServerStartup, ARefusedDatabaseUrlIsNeverEchoedBecauseItHoldsThePassword) {
    for (const std::string url :
         {"postgres://ulw:hunt%zzer2@db/ulw", "host=db password=hunt%zzer2 dbname='ulw"}) {
        auto chat = ChildProcess::start(
            {ULW_CHAT_BIN},
            {"ULW_NODE_ID=chat-1", "ULW_NODE_ADDRESS=127.0.0.1:9201",
             "ULW_NODE_SECRET=startup-test-node-secret-000000000000", "ULW_DEV_LOOPBACK_NODES=1",
             "ULW_DATABASE_URL=" + url, "ULW_DEV_JWKS_FILE=/nonexistent/jwks.json",
             "ULW_DEV_MODE=1", "JWT_ISSUER=https://issuer.test", "ULW_ALLOW_ROOT=1"});
        ASSERT_NE(chat, nullptr);
        EXPECT_EQ(chat->wait_exit(seconds(30)), 2) << chat->output();
        EXPECT_NE(chat->output().find("ULW_DATABASE_URL"), std::string::npos) << chat->output();
        EXPECT_EQ(chat->output().find("hunt"), std::string::npos) << chat->output();
    }
}

// A call's ticket, as the room WebSocket answers a call (docs/integration/calls.md): the ticket,
// or an error.
std::optional<Seen> call_answer(Client& client, const std::string& room,
                                const std::string& device) {
    const auto answers = [&](const Seen& s) {
        return s.room == room && (s.type == "ticket" || s.type == "error");
    };
    // This call's answer, not one an earlier call of the client's got.
    std::size_t earlier = client.count(answers);
    if (!client.send(R"({"type":"call","room":")" + room + R"(","device":")" + device + R"("})")) {
        return std::nullopt;
    }
    return client.wait_for([&](const Seen& s) {
        if (!answers(s)) {
            return false;
        }
        if (earlier > 0) {
            --earlier;
            return false;
        }
        return true;
    });
}

// LIVEKIT_CLIENT_URL, what every ticket names; empty when unset.
std::string livekit_client_url() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
    const char* url = std::getenv("LIVEKIT_CLIENT_URL");
    return url == nullptr ? std::string() : std::string(url);
}

// The port LiveKit's client URL names, which this test reaches it on: ws://127.0.0.1:<port>.
std::uint16_t livekit_port() {
    const std::string url = livekit_client_url();
    return core::parse_integer<std::uint16_t>(url.substr(url.rfind(':') + 1)).value_or(0);
}

// A client on LiveKit's signalling socket, with what LiveKit sent it first once its ticket
// admitted it: the join response (protobuf, whose strings appear as they are). nullopt when
// LiveKit refused the ticket, with its status line in `refusal`.
struct RtcClient {
    ulw::test::WsClient socket;
    std::string join;
};

std::optional<RtcClient> rtc_join(const std::string& token, std::string* refusal) {
    auto rtc = ulw::test::WsClient::connect(
        livekit_port(), "/rtc?access_token=" + token + "&auto_subscribe=1&sdk=js&protocol=15", "",
        refusal);
    if (!rtc) {
        return std::nullopt;
    }
    while (auto frame = rtc->next_frame(std::chrono::seconds(10))) {
        if (frame->first == codec::ws::Opcode::Binary) {
            return RtcClient{.socket = std::move(*rtc), .join = std::move(frame->second)};
        }
    }
    return std::nullopt;
}

// M23 to M26 through chat (ADR-0050): each member of a direct chat asks for the call on the room
// WebSocket of whichever node it is on; the room's owner answers both, and LiveKit admits each
// ticket into the same room, where the second finds the first. Needs a LiveKit
// (LIVEKIT_API_URL and the rest, as tests/call/run.sh sets them); skipped without one.
// Member lists their users change (ADR-0096), across the cluster: a direct chat opened on one
// node is the same room on every node, its pair hear they were listed wherever they are
// connected, and nobody else is let in.
TEST_P(ChatClusterTest, ADirectChatOpenedOnOneNodeIsTheSameRoomOnEveryOtherAndTheirsAlone) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && bob && carol);
    const std::string room = open_direct(*alice, "bob");
    ASSERT_EQ(room.find("error"), std::string::npos) << room;
    ASSERT_EQ(room.substr(0, 2), "03") << "not a pair's room";
    // Bob, on another node, hears he was listed, without having asked for anything.
    const auto told = bob->wait_for(
        [&](const Seen& s) { return s.type == "member" && s.room == room && s.user == "bob"; });
    ASSERT_TRUE(told);
    EXPECT_EQ(told->change, "added");
    // From his side it is the same room, and opening it again lists nobody more.
    EXPECT_EQ(open_direct(*bob, "alice"), room);
    EXPECT_EQ(open_direct(*carol, "carol"), "error: self");

    EXPECT_EQ(join_again(*alice, room), "joined");
    EXPECT_EQ(join_again(*bob, room), "joined");
    EXPECT_EQ(join_again(*carol, room), "not_member");
    // A join that calls it a group names the wrong room.
    EXPECT_EQ(join_again(*carol, room, R"(,"kind":"group")"), "bad_room");
    ASSERT_TRUE(alice->send(send_command(room, "hello bob", "d1")));
    ASSERT_TRUE(bob->message("hello bob"));
    EXPECT_FALSE(carol->ever_saw("hello bob"));
    EXPECT_EQ(metric(nodes_[0], "directs_opened_total") + metric(nodes_[1], "directs_opened_total"),
              1U);

    // Bob's list holds the room, with alice as its other member.
    const auto rooms = ask(*bob, R"({"type":"rooms"})", "rooms");
    ASSERT_TRUE(rooms);
    EXPECT_NE(rooms->raw.find(R"({"room":")" + room +
                              R"(","kind":"direct","role":"member","peer":"alice"})"),
              std::string::npos)
        << rooms->raw;
    // A direct chat's pair is fixed: nobody is added, nobody leaves.
    EXPECT_EQ(
        ask(*alice, R"({"type":"add_members","room":")" + room + R"(","users":["carol"]})", "added")
            ->reason,
        "not_group");
    EXPECT_EQ(ask(*alice, R"({"type":"leave","room":")" + room + R"("})", "left")->reason,
              "not_group");
}

// A group made on one node, changed by its admin on another: each change reaches the user it
// names and everyone in the room, whichever node they are on, and a removed member is out of the
// room at once.
TEST_P(ChatClusterTest, AGroupsAdminChangesItsListAndEveryNodeActsOnIt) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    auto dave = connect(nodes_[2], 3);
    ASSERT_TRUE(alice && bob && carol && dave);
    const auto created =
        ask(*alice, R"({"type":"create_group","id":"team-1","users":["bob"]})", "group");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->type, "group") << created->reason;
    const std::string room = created->room;
    EXPECT_EQ(created->id, "team-1");
    // The same request again names the same room.
    EXPECT_EQ(
        ask(*alice, R"({"type":"create_group","id":"team-1","users":["bob"]})", "group")->room,
        room);
    EXPECT_EQ(join_again(*alice, room), "joined");
    EXPECT_EQ(join_again(*bob, room), "joined");

    // Only the admin adds.
    EXPECT_EQ(
        ask(*bob, R"({"type":"add_members","room":")" + room + R"(","users":["carol"]})", "added")
            ->reason,
        "not_admin");
    const auto added =
        ask(*alice, R"({"type":"add_members","room":")" + room + R"(","users":["carol","bob"]})",
            "added");
    ASSERT_TRUE(added);
    EXPECT_NE(added->raw.find(R"("users":["carol"])"), std::string::npos) << added->raw;
    // Carol hears she was listed on her node, and bob, in the room, on his.
    ASSERT_TRUE(carol->wait_for([&](const Seen& s) {
        return s.type == "member" && s.user == "carol" && s.change == "added";
    }));
    ASSERT_TRUE(bob->wait_for([&](const Seen& s) {
        return s.type == "member" && s.user == "carol" && s.change == "added";
    }));
    // Dave, on carol's node, is neither named nor in the room: the node told carol, and nothing
    // reached him before the answer to what he asks next.
    ASSERT_TRUE(ask(*dave, R"({"type":"rooms"})", "rooms"));
    EXPECT_EQ(dave->count([](const Seen& s) { return s.type == "member"; }), 0U)
        << "dave heard of a room he is not in";
    EXPECT_EQ(join_again(*carol, room), "joined");
    ASSERT_TRUE(alice->send(send_command(room, "welcome carol", "g1")));
    ASSERT_TRUE(carol->message("welcome carol"));

    const auto members = ask(*carol, R"({"type":"members","room":")" + room + R"("})", "members");
    ASSERT_TRUE(members);
    EXPECT_NE(
        members->raw.find(
            R"("members":[{"user":"alice","role":"admin"},{"user":"bob","role":"member"},{"user":"carol","role":"member"}],"more":false)"),
        std::string::npos)
        << members->raw;
    EXPECT_EQ(ask(*dave, R"({"type":"members","room":")" + room + R"("})", "members")->reason,
              "not_member");

    // Removed by the admin on node 1, carol is out of the room on node 3 at once.
    const auto removed = ask(
        *alice, R"({"type":"remove_member","room":")" + room + R"(","user":"carol"})", "removed");
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->type, "removed") << removed->reason;
    ASSERT_TRUE(carol->wait_for([&](const Seen& s) {
        return s.type == "error" && s.reason == "not_member" && s.room == room;
    }));
    ASSERT_TRUE(bob->wait_for([&](const Seen& s) {
        return s.type == "member" && s.user == "carol" && s.change == "removed";
    }));
    ASSERT_TRUE(alice->send(send_command(room, "after carol", "g2")));
    ASSERT_TRUE(bob->message("after carol"));
    EXPECT_FALSE(carol->ever_saw("after carol"));
    EXPECT_EQ(join_again(*carol, room), "not_member");

    // The admin leaves; bob, the one left, is its admin now, on whichever node he asks.
    // Her socket in the room is taken out of it as any removed member's is, told not_member and
    // the change, before or after the answer: the notification and the commit's reply race.
    const std::size_t before_leave = alice->seen().size();
    ASSERT_TRUE(alice->send(R"({"type":"leave","room":")" + room + R"("})"));
    const auto left =
        alice->wait_from(before_leave, [](const Seen& s) { return s.type == "left"; });
    ASSERT_TRUE(left);
    EXPECT_NE(alice->seen()[*left].raw.find(R"("promoted":"bob")"), std::string::npos)
        << alice->seen()[*left].raw;
    ASSERT_TRUE(alice->wait_from(before_leave, [&](const Seen& s) {
        return s.type == "error" && s.reason == "not_member" && s.room == room;
    }));
    ASSERT_TRUE(alice->wait_from(before_leave, [&](const Seen& s) {
        return s.type == "member" && s.user == "alice" && s.change == "removed";
    }));
    // Bob, on another node, hears it from the database as everyone in the room would.
    ASSERT_TRUE(bob->wait_for([&](const Seen& s) {
        return s.type == "member" && s.user == "bob" && s.change == "promoted";
    }));
    const auto now = ask(*bob, R"({"type":"members","room":")" + room + R"("})", "members");
    ASSERT_TRUE(now);
    EXPECT_NE(now->raw.find(R"("members":[{"user":"bob","role":"admin"}])"), std::string::npos)
        << now->raw;
    EXPECT_EQ(metric(nodes_[0], "groups_created_total"), 1U);
    EXPECT_EQ(metric(nodes_[0], "members_changed_total{change=\"added\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "members_changed_total{change=\"removed\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "members_changed_total{change=\"left\"}"), 1U);
    EXPECT_EQ(metric(nodes_[1], "membership_refusals_total{reason=\"not_admin\"}"), 1U);
}

TEST_P(ChatClusterTest, ADirectChatsMembersGetTicketsOnAnyNodeThatLiveKitAdmitsToOneRoom) {
    if (ulw::test::livekit_environment().empty()) {
        GTEST_SKIP() << "no LiveKit: set LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY "
                        "and LIVEKIT_API_SECRET";
    }
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    // Opened by alice, as a client does (ADR-0096).
    const std::string direct = open_direct(*alice, "bob");
    ASSERT_EQ(direct.substr(0, 2), "03") << direct;
    // Alice's join makes chat-1 the room's owner; bob's node forwards his call to it.
    ASSERT_EQ(join_answer(*alice, direct), "joined");
    ASSERT_EQ(join_answer(*bob, direct), "joined");
    // Fixed and distinct: two devices are two participants only if their ids differ.
    const std::string alice_device = "01a0eb86-6cca-7dce-84cc-3bb47615f9a1";
    const std::string bob_device = "01a0eb86-6cca-7dce-84cc-3bb47615f9b1";
    const auto issued = std::chrono::duration_cast<seconds>(clock_.wall_now().time_since_epoch());
    const auto a = call_answer(*alice, direct, alice_device);
    const auto b = call_answer(*bob, direct, bob_device);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->type, "ticket") << a->reason;
    ASSERT_EQ(b->type, "ticket") << b->reason;
    EXPECT_EQ(a->url, livekit_client_url());
    EXPECT_EQ(b->url, a->url);
    // A minute to connect with (ADR-0050).
    EXPECT_GE(a->expires_at, static_cast<std::uint64_t>(issued.count()) + 59);
    EXPECT_LE(a->expires_at, static_cast<std::uint64_t>(issued.count()) + 61);
    EXPECT_EQ(metric(nodes_[0], "call_tickets_total"), 2U);
    EXPECT_EQ(metric(nodes_[1], "call_tickets_total"), 0U);
    EXPECT_EQ(metric(nodes_[0], "call_rooms_opened_total"), 1U);

    std::string refusal;
    const auto first = rtc_join(a->token, &refusal);
    ASSERT_TRUE(first) << "LiveKit refused alice's ticket: " << refusal;
    EXPECT_NE(first->join.find(direct + ":1"), std::string::npos);
    EXPECT_NE(first->join.find("alice/" + alice_device), std::string::npos);
    const auto second = rtc_join(b->token, &refusal);
    ASSERT_TRUE(second) << "LiveKit refused bob's ticket: " << refusal;
    EXPECT_NE(second->join.find(direct + ":1"), std::string::npos);
    EXPECT_NE(second->join.find("bob/" + bob_device), std::string::npos);
    EXPECT_NE(second->join.find("alice/" + alice_device), std::string::npos)
        << "bob's join response does not name alice as already in the room";

    // A call is two participants: a third device, with a ticket of its own, is refused.
    const auto third = call_answer(*alice, direct, "01a0eb86-6cca-7dce-84cc-3bb47615f9a2");
    ASSERT_TRUE(third);
    ASSERT_EQ(third->type, "ticket") << third->reason;
    EXPECT_FALSE(rtc_join(third->token, &refusal)) << "LiveKit admitted a third participant";

    // The ticket is what admits: one with its signature altered is refused.
    // The signature's first character: all six of its bits are the signature's, where the
    // last character's lowest two are padding.
    std::string forged = a->token;
    char& signed_char = forged.at(forged.rfind('.') + 1);
    signed_char = signed_char == 'A' ? 'B' : 'A';
    EXPECT_FALSE(rtc_join(forged, &refusal));
    EXPECT_NE(refusal.find(" 401"), std::string::npos) << refusal;
}

// Waits for `client` to hear `type` for `call` (any call when empty) after the `from`th thing it
// heard; where it heard it, so that the next wait starts past it.
std::optional<std::size_t> heard(Client& client, std::size_t from, const std::string& type,
                                 const std::string& call = {}) {
    return client.wait_from(
        from, [&](const Seen& s) { return s.type == type && (call.empty() || s.call == call); });
}

// A call's decline, cancel or end on the room WebSocket.
std::string move_command(const std::string& type, const std::string& room,
                         const std::string& call) {
    return R"({"type":")" + type + R"(","room":")" + room + R"(","call":")" + call + R"("})";
}

// The M23 to M26 calls made usable (ADR-0091): alice, on one node, calls bob, connected to another
// node twice and in no room at all. Every socket of both hears the ring, and how it ends: bob
// answering on one device (the other stops ringing), then ending it; bob declining; alice
// cancelling; and a ring nobody answers running out for both. Needs a LiveKit, as the tickets do.
TEST_P(ChatClusterTest, ACallRingsTheOtherMembersEverySocketOnAnyNodeAndEndsOnceForBoth) {
    if (ulw::test::livekit_environment().empty()) {
        GTEST_SKIP() << "no LiveKit: set LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY "
                        "and LIVEKIT_API_SECRET";
    }
    const std::string direct = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(direct, {"alice", "bob"}, "direct_chat"));
    auto alice = connect(nodes_[0], 0);
    auto phone = connect(nodes_[1], 1);
    auto laptop = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && phone && laptop);
    // Each hears the other come online: both nodes are then in both presence rooms the ring's
    // notices travel through.
    const auto online = [](const std::string& who) {
        return [who](const Seen& s) {
            return (s.type == "watching" || s.type == "presence") && s.user == who &&
                   s.status == "online";
        };
    };
    ASSERT_TRUE(alice->send(R"({"type":"watch","user":"bob"})"));
    ASSERT_TRUE(phone->send(R"({"type":"watch","user":"alice"})"));
    ASSERT_TRUE(alice->wait_for(online("bob")));
    ASSERT_TRUE(phone->wait_for(online("alice")));
    ASSERT_EQ(join_answer(*alice, direct), "joined");
    const std::string alice_device = "01a0eb86-6cca-7dce-84cc-3bb47615f9a1";
    const std::string phone_device = "01a0eb86-6cca-7dce-84cc-3bb47615f9b1";
    // A socket, and where in what it heard the next wait starts.
    using Ear = std::pair<Client*, std::size_t*>;
    std::size_t at_alice = alice->seen().size();
    std::size_t at_phone = phone->seen().size();
    std::size_t at_laptop = laptop->seen().size();
    // Every wait below starts past what that client last matched.
    const auto expect = [](Client& c, std::size_t& at, const std::string& type,
                           const std::string& call) -> std::optional<Seen> {
        const auto where = heard(c, at, type, call);
        if (!where) {
            ADD_FAILURE() << c.name() << " never heard " << type;
            return std::nullopt;
        }
        at = *where + 1;
        return c.seen()[*where];
    };

    // 1. Alice calls; bob answers on his phone; the laptop stops ringing; alice ends it.
    const auto ticket = call_answer(*alice, direct, alice_device);
    ASSERT_TRUE(ticket);
    ASSERT_EQ(ticket->type, "ticket") << ticket->reason;
    const std::string first = ticket->call;
    ASSERT_FALSE(first.empty()) << "the ticket names no call";
    for (const auto& [client, at] : std::initializer_list<Ear>{
             {laptop.get(), &at_laptop}, {phone.get(), &at_phone}, {alice.get(), &at_alice}}) {
        const auto ringing = expect(*client, *at, "call_ringing", first);
        ASSERT_TRUE(ringing);
        EXPECT_EQ(ringing->room, direct);
        EXPECT_EQ(ringing->from, "alice");
        EXPECT_GE(ringing->expires_at, ticket->expires_at - 60 + 2);
    }
    // The phone was in no room: it joins the direct chat to answer.
    ASSERT_EQ(join_answer(*phone, direct), "joined");
    const auto answer = call_answer(*phone, direct, phone_device);
    ASSERT_TRUE(answer);
    ASSERT_EQ(answer->type, "ticket") << answer->reason;
    EXPECT_EQ(answer->call, first);
    for (const auto& [client, at] :
         std::initializer_list<Ear>{{alice.get(), &at_alice}, {laptop.get(), &at_laptop}}) {
        const auto answered = expect(*client, *at, "call_answered", first);
        ASSERT_TRUE(answered);
        EXPECT_EQ(answered->by, "bob");
    }
    ASSERT_TRUE(alice->send(move_command("call_end", direct, first)));
    for (const auto& [client, at] : std::initializer_list<Ear>{
             {laptop.get(), &at_laptop}, {phone.get(), &at_phone}, {alice.get(), &at_alice}}) {
        const auto ended = expect(*client, *at, "call_ended", first);
        ASSERT_TRUE(ended);
        EXPECT_EQ(ended->by, "alice");
    }

    // 2. Alice calls and gives up before anyone answers.
    const auto cancelled_call = call_answer(*alice, direct, alice_device);
    ASSERT_TRUE(cancelled_call && cancelled_call->type == "ticket");
    ASSERT_TRUE(expect(*phone, at_phone, "call_ringing", cancelled_call->call));
    ASSERT_TRUE(alice->send(move_command("call_cancel", direct, cancelled_call->call)));
    for (const auto& [client, at] :
         std::initializer_list<Ear>{{phone.get(), &at_phone}, {laptop.get(), &at_laptop}}) {
        const auto cancelled = expect(*client, *at, "call_cancelled", cancelled_call->call);
        ASSERT_TRUE(cancelled);
        EXPECT_EQ(cancelled->by, "alice");
    }

    // 3. Nobody answers: after the ring timeout both are told it was missed, once.
    const auto missed_call = call_answer(*alice, direct, alice_device);
    ASSERT_TRUE(missed_call && missed_call->type == "ticket");
    for (const auto& [client, at] : std::initializer_list<Ear>{
             {alice.get(), &at_alice}, {phone.get(), &at_phone}, {laptop.get(), &at_laptop}}) {
        ASSERT_TRUE(expect(*client, *at, "call_missed", missed_call->call));
    }

    // 4. Alice calls again: a new call, which bob declines from the laptop.
    const auto again = call_answer(*alice, direct, alice_device);
    ASSERT_TRUE(again && again->type == "ticket");
    const std::string second = again->call;
    EXPECT_NE(second, first);
    ASSERT_TRUE(expect(*laptop, at_laptop, "call_ringing", second));
    ASSERT_TRUE(expect(*phone, at_phone, "call_ringing", second));
    ASSERT_EQ(join_answer(*laptop, direct), "joined");
    ASSERT_TRUE(laptop->send(move_command("call_decline", direct, second)));
    for (const auto& [client, at] : std::initializer_list<Ear>{
             {alice.get(), &at_alice}, {phone.get(), &at_phone}, {laptop.get(), &at_laptop}}) {
        const auto declined = expect(*client, *at, "call_declined", second);
        ASSERT_TRUE(declined);
        EXPECT_EQ(declined->by, "bob");
    }
    // Declining twice finds nothing to decline.
    ASSERT_TRUE(laptop->send(move_command("call_decline", direct, second)));
    const auto twice =
        laptop->wait_from(at_laptop, [](const Seen& s) { return s.type == "error"; });
    ASSERT_TRUE(twice);
    EXPECT_EQ(laptop->seen()[*twice].reason, "no_call");

    // Declined, alice may not ring bob again straight away; nothing rings, and she is told when.
    const auto too_soon = call_answer(*alice, direct, alice_device);
    ASSERT_TRUE(too_soon);
    EXPECT_EQ(too_soon->type, "error");
    EXPECT_EQ(too_soon->reason, "ring_limited");
    ASSERT_TRUE(too_soon->retry_after_ms);
    EXPECT_GT(*too_soon->retry_after_ms, 20'000U);
    EXPECT_LE(*too_soon->retry_after_ms, 30'000U);

    // The ring is the owner's (chat-1, where alice joined first): every notice went from it,
    // through the presence rooms, to the nodes the members are on.
    EXPECT_EQ(metric(nodes_[0], "call_rings_total{outcome=\"started\"}"), 4U);
    EXPECT_EQ(metric(nodes_[0], "call_rings_total{outcome=\"answered\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "call_rings_total{outcome=\"declined\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "call_rings_total{outcome=\"cancelled\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "call_rings_total{outcome=\"missed\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "call_rings_total{outcome=\"ended\"}"), 1U);
    EXPECT_EQ(metric(nodes_[0], "call_refusals_total{reason=\"ring_limited\"}"), 1U);
    EXPECT_EQ(metric(nodes_[1], "call_rings_total{outcome=\"started\"}"), 0U);
    EXPECT_GE(metric(nodes_[1], "call_events_pushed_total"), 10U);
}

// Whether LiveKit ends `rtc`'s connection within `limit`: what a client in a closed generation
// sees.
bool rtc_ended(RtcClient& rtc, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto frame =
            rtc.socket.next_frame(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
        if (!frame) {
            return !rtc.socket.connected();
        }
        if (frame->first == codec::ws::Opcode::Close) {
            return true;
        }
    }
    return false;
}

// A group call expel on the room WebSocket.
std::string expel_command(const std::string& room, const std::string& call,
                          const std::string& user) {
    return R"({"type":"call_expel","room":")" + room + R"(","call":")" + call + R"(","user":")" +
           user + R"("})";
}

// M27 through chat (ADR-0095): four members of a group chat, on all three nodes, ask for the call
// and LiveKit admits all four to one room, the fourth finding the other three in it; a fifth
// device finds it full. The caller puts one out: the generation moves on, LiveKit closes the old
// one under everyone, the others are let into the next with fresh tickets, and the one put out
// is refused, by chat and by LiveKit. The owner is then killed: within a bounded time another
// node owns the room and tickets the same generation, and a member removed from the chat is put
// out of the call by it. Needs a LiveKit, as the tickets do.
TEST_P(ChatClusterTest, AGroupCallOfFourAcrossNodesPutsOneOutAndOutlivesItsOwner) {
    if (ulw::test::livekit_environment().empty()) {
        GTEST_SKIP() << "no LiveKit: set LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY "
                        "and LIVEKIT_API_SECRET";
    }
    const std::string group = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(group, {"alice", "bob", "carol", "dave"}));
    // Alice's join makes chat-1 the owner; the others are on chat-2 and chat-3.
    std::vector<std::unique_ptr<Client>> clients;
    clients.push_back(connect(nodes_[0], 0));
    clients.push_back(connect(nodes_[1], 1));
    clients.push_back(connect(nodes_[2], 2));
    clients.push_back(connect(nodes_[1], 3));
    for (const auto& c : clients) {
        ASSERT_TRUE(c);
    }
    Client& alice = *clients[0];
    Client& bob = *clients[1];
    Client& carol = *clients[2];
    Client& dave = *clients[3];
    // Alice hears each of them online: their presence rooms, which carry the ring, are settled.
    for (const char* who : {"bob", "carol", "dave"}) {
        ASSERT_TRUE(alice.send(std::string(R"({"type":"watch","user":")") + who + R"("})"));
        ASSERT_TRUE(alice.wait_for([&](const Seen& s) {
            return (s.type == "watching" || s.type == "presence") && s.user == who &&
                   s.status == "online";
        }));
    }
    for (const auto& c : clients) {
        ASSERT_EQ(join_answer(*c, group), "joined");
    }
    const std::array<std::string, 4> devices{
        "01a0eb86-6cca-7dce-84cc-3bb47615f9a1", "01a0eb86-6cca-7dce-84cc-3bb47615f9b1",
        "01a0eb86-6cca-7dce-84cc-3bb47615f9c1", "01a0eb86-6cca-7dce-84cc-3bb47615f9d1"};
    std::array<std::size_t, 4> at{};
    for (std::size_t i = 0; i < clients.size(); ++i) {
        at.at(i) = clients[i]->seen().size();
    }
    const auto expect = [&](std::size_t who, const std::string& type,
                            const std::string& call) -> std::optional<Seen> {
        Client& c = *clients.at(who);
        const auto where = heard(c, at.at(who), type, call);
        if (!where) {
            ADD_FAILURE() << c.name() << " never heard " << type;
            return std::nullopt;
        }
        at.at(who) = *where + 1;
        return c.seen()[*where];
    };

    // The call rings the three others; each answers with a ticket, and everyone hears it.
    const auto first = call_answer(alice, group, devices[0]);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->type, "ticket") << first->reason;
    const std::string call = first->call;
    for (std::size_t i = 1; i < 4; ++i) {
        const auto ringing = expect(i, "call_ringing", call);
        ASSERT_TRUE(ringing);
        EXPECT_EQ(ringing->from, "alice");
    }
    std::vector<std::string> tokens{first->token};
    for (std::size_t i = 1; i < 4; ++i) {
        const auto t = call_answer(*clients[i], group, devices.at(i));
        ASSERT_TRUE(t);
        ASSERT_EQ(t->type, "ticket") << t->reason;
        EXPECT_EQ(t->call, call);
        tokens.push_back(t->token);
        const auto answered = expect(0, "call_answered", call);
        ASSERT_TRUE(answered);
        EXPECT_EQ(answered->by, clients[i]->name());
    }
    EXPECT_EQ(metric(nodes_[0], "call_tickets_total"), 4U);

    // LiveKit admits the four to one room; the fourth finds the other three there.
    std::vector<RtcClient> rtc;
    std::string refusal;
    for (std::size_t i = 0; i < 4; ++i) {
        auto joined = rtc_join(tokens[i], &refusal);
        ASSERT_TRUE(joined) << "LiveKit refused " << clients[i]->name() << ": " << refusal;
        EXPECT_NE(joined->join.find(group + ":1"), std::string::npos);
        rtc.push_back(std::move(*joined));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NE(rtc[3].join.find(clients[i]->name() + "/" + devices.at(i)), std::string::npos)
            << "dave's join response does not name " << clients[i]->name();
    }
    // Four devices fill it: chat counts them and refuses a fifth before LiveKit has to.
    const auto fifth = call_answer(alice, group, "01a0eb86-6cca-7dce-84cc-3bb47615f9a2");
    ASSERT_TRUE(fifth);
    EXPECT_EQ(fifth->type, "error");
    EXPECT_EQ(fifth->reason, "call_full");
    // Only the caller puts anyone out.
    ASSERT_TRUE(bob.send(expel_command(group, call, "carol")));
    const auto refused = bob.wait_from(at[1], [](const Seen& s) { return s.type == "error"; });
    ASSERT_TRUE(refused);
    EXPECT_EQ(bob.seen()[*refused].reason, "no_call");
    at[1] = *refused + 1;

    // Alice puts dave out: everyone hears the move, and LiveKit closes generation 1 under them.
    const auto expelled_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(alice.send(expel_command(group, call, "dave")));
    for (std::size_t i = 0; i < 4; ++i) {
        const auto moved = expect(i, "call_moved", call);
        ASSERT_TRUE(moved);
        EXPECT_EQ(moved->expelled, "dave");
        EXPECT_EQ(moved->by, "alice");
    }
    EXPECT_TRUE(rtc_ended(rtc[3], std::chrono::seconds(10))) << "dave stayed in generation 1";
    EXPECT_TRUE(rtc_ended(rtc[0], std::chrono::seconds(10)));
    const auto closed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - expelled_at);
    EXPECT_EQ(metric(nodes_[0], "call_moves_total{reason=\"expel\"}"), 1U);
    // Neither chat nor LiveKit lets dave back in.
    const auto dave_again = call_answer(dave, group, devices[3]);
    ASSERT_TRUE(dave_again);
    EXPECT_EQ(dave_again->reason, "expelled");
    EXPECT_FALSE(rtc_join(tokens[3], &refusal)) << "a ticket for the closed generation admitted";
    // The others come back into generation 2 with fresh tickets.
    rtc.clear();
    for (std::size_t i = 0; i < 3; ++i) {
        const auto t = call_answer(*clients[i], group, devices.at(i));
        ASSERT_TRUE(t);
        ASSERT_EQ(t->type, "ticket") << t->reason;
        auto joined = rtc_join(t->token, &refusal);
        ASSERT_TRUE(joined) << refusal;
        EXPECT_NE(joined->join.find(group + ":2"), std::string::npos);
        rtc.push_back(std::move(*joined));
    }
    const auto back_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - expelled_at);
    std::cout << "expelled: generation 1 closed after " << closed_ms.count()
              << " ms, the other three back in generation 2 after " << back_ms.count() << " ms\n";

    // The owner dies. Another node takes the room once its heartbeat is stale, and tickets the
    // generation the store holds, not the one a call starts at.
    nodes_[0].process->signal(SIGKILL);
    const auto killed_at = std::chrono::steady_clock::now();
    std::optional<Seen> after;
    ASSERT_TRUE(nodes_[1].process->poll_until(
        [&] {
            after = call_answer(bob, group, devices[1]);
            return after && after->type == "ticket";
        },
        std::chrono::seconds(30), std::chrono::milliseconds(500)))
        << (after ? after->reason : "no answer");
    const auto takeover_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - killed_at);
    std::cout << "owner killed: a ticket again after " << takeover_ms.count() << " ms\n";
    // ADR-0015's stale bound (5 s) and the router's look-up, with the poll's period on top.
    EXPECT_LT(takeover_ms, std::chrono::seconds(15));
    auto rejoined = rtc_join(after->token, &refusal);
    ASSERT_TRUE(rejoined) << refusal;
    EXPECT_NE(rejoined->join.find(group + ":2"), std::string::npos)
        << "the new owner ticketed another generation";
    // Media never passed through the owner: carol's connection to LiveKit is still up.
    EXPECT_TRUE(rtc[2].socket.connected());
    // The new owner knew no call: bob's ticket started one, which carol answers.
    const std::string second = after->call;
    ASSERT_FALSE(second.empty());
    ASSERT_TRUE(expect(2, "call_ringing", second));
    const auto carol_ticket = call_answer(carol, group, devices[2]);
    ASSERT_TRUE(carol_ticket && carol_ticket->type == "ticket");
    ASSERT_TRUE(expect(1, "call_answered", second));

    // Carol leaves the chat: the new owner puts her out of the call, moving to generation 3.
    {
        auto conn = db_->session();
        ASSERT_TRUE(conn.exec(
            "DELETE FROM chat_members WHERE room_id = $1::text::uuid AND user_id = 'carol'",
            infra::postgres::Params{}.add_text(group)));
    }
    const auto removed = expect(1, "call_moved", second);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->expelled, "carol");
    EXPECT_TRUE(removed->by.empty());
    EXPECT_TRUE(rtc_ended(rtc[2], std::chrono::seconds(10))) << "carol stayed in generation 2";
    const auto bob_again = call_answer(bob, group, devices[1]);
    ASSERT_TRUE(bob_again && bob_again->type == "ticket");
    auto third = rtc_join(bob_again->token, &refusal);
    ASSERT_TRUE(third) << refusal;
    EXPECT_NE(third->join.find(group + ":3"), std::string::npos);

    // Bob leaves, and with nobody else in the call, it ends for everyone still listening.
    ASSERT_TRUE(bob.send(move_command("call_leave", group, second)));
    const auto left = expect(1, "call_left", second);
    ASSERT_TRUE(left);
    EXPECT_EQ(left->by, "bob");
    const auto ended = expect(1, "call_ended", second);
    ASSERT_TRUE(ended);
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatClusterTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
