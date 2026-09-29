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

#include "chat_cluster.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::Client;
using ulw::test::Node;
using ulw::test::Seen;

class ChatClusterTest : public ulw::test::ChatCluster {};

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

    // The survivors carry on through the new owner.
    ASSERT_TRUE(send_until_heard(*bob, "bob after the failover"));
    ASSERT_TRUE(send_until_heard(*carol, "carol after the failover"));
    ASSERT_TRUE(bob->message(last_body_["carol"]));
    ASSERT_TRUE(carol->message(last_body_["bob"]));
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
          "carol after the failover", "alice is back"}) {
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

    auto again = connect(nodes_[1], 0);
    ASSERT_TRUE(again);
    ASSERT_NO_FATAL_FAILURE(join(*again));
    ASSERT_TRUE(again->send(send_command(room_, body, "kept-key")));
    const auto repeat =
        again->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "kept-key"; });
    ASSERT_TRUE(repeat);
    EXPECT_EQ(repeat->seq, ack->seq);
    ASSERT_TRUE(again->send(send_command(room_, "another body", "kept-key")));
    const auto conflict =
        again->wait_for([](const Seen& s) { return s.type == "error" && s.id == "kept-key"; });
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->reason, "conflict");
    ASSERT_TRUE(again->send(send_command(room_, "after the resends", "marker")));
    ASSERT_TRUE(again->message("after the resends"));
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
              << " from the store, and another body under its id was a conflict\n";
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

TEST_P(ChatClusterTest, AGroupRoomWithNoMembersRefusesEveryoneAndCannotBeOpenedLater) {
    const std::string nobody = core::RoomId::generate(clock_, random_).to_string();
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    EXPECT_EQ(join_answer(*alice, nobody), "not_member");
    // Its first join recorded it as a group chat; asking for live afterwards opens nothing.
    EXPECT_EQ(join_answer(*bob, nobody, R"(,"kind":"live")"), "not_member");
}

TEST_P(ChatClusterTest, ALiveRoomAdmitsAnyone) {
    const std::string live = core::RoomId::generate(clock_, random_).to_string();
    auto alice = connect(nodes_[0], 0);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && carol);
    EXPECT_EQ(join_answer(*alice, live, R"(,"kind":"live")"), "joined");
    // Joins after the first need not know what the room is.
    EXPECT_EQ(join_answer(*carol, live), "joined");
    ASSERT_TRUE(carol->send(send_command(live, "hello, stream", "live-1")));
    ASSERT_TRUE(alice->message("hello, stream"));
}

// No database is reached: the connection string is refused before any connection is tried.
TEST(ChatServerStartup, ARefusedDatabaseUrlIsNeverEchoedBecauseItHoldsThePassword) {
    for (const std::string url :
         {"postgres://ulw:hunt%zzer2@db/ulw", "host=db password=hunt%zzer2 dbname='ulw"}) {
        auto chat = ChildProcess::start({ULW_CHAT_BIN},
                                        {"ULW_NODE_ID=chat-1", "ULW_NODE_ADDRESS=127.0.0.1:9201",
                                         "ULW_NODE_SECRET=startup-test-node-secret-000000000000",
                                         "ULW_DEV_LOOPBACK_NODES=1", "ULW_DATABASE_URL=" + url,
                                         "ULW_DEV_JWKS_FILE=/nonexistent/jwks.json",
                                         "JWT_ISSUER=https://issuer.test"});
        ASSERT_NE(chat, nullptr);
        EXPECT_EQ(chat->wait_exit(seconds(30)), 2) << chat->output();
        EXPECT_NE(chat->output().find("ULW_DATABASE_URL"), std::string::npos) << chat->output();
        EXPECT_EQ(chat->output().find("hunt"), std::string::npos) << chat->output();
    }
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatClusterTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
