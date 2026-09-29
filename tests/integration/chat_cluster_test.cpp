// The M16 acceptance: three chat_server processes on one Postgres. Clients on different nodes
// see each other's messages; a stopped owner is replaced under a higher owner_generation; once
// resumed, its stale write updates no rows, is logged as "fenced out", and reaches nobody.
// ULW_CHAT_CLUSTER_PORTS=9101,9102,9103 pins the client ports (the CI job does); otherwise
// free ones are taken.

#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/eventually.hpp"
#include "support/reactor_harness.hpp"
#include "support/temp_dir.hpp"
#include "support/ws_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using infra::postgres::Params;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::ScratchDatabase;
using ulw::test::WsClient;

constexpr std::string_view kIssuer = "https://auth.test.askedin.com";

std::uint16_t free_port() {
    const os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    // bind() and getsockname() take every address family through the generic header.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return ntohs(addr.sin_port);
}

std::vector<std::uint16_t> client_ports() {
    std::vector<std::uint16_t> ports;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
    const char* pinned = std::getenv("ULW_CHAT_CLUSTER_PORTS");
    std::string_view rest = pinned == nullptr ? "" : pinned;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        ports.push_back(core::parse_integer<std::uint16_t>(rest.substr(0, comma)).value_or(0));
        rest = comma == std::string_view::npos ? "" : rest.substr(comma + 1);
    }
    while (ports.size() < 3) {
        ports.push_back(free_port());
    }
    return ports;
}

// A server message, the fields the test looks at.
struct Seen {
    std::string type;
    std::uint64_t seq = 0;
    std::string sender;
    std::string body;
    std::string reason;
    std::optional<std::uint64_t> ref;
};

std::optional<Seen> parse_seen(const std::string& text) {
    const auto json = core::json::parse(text);
    if (!json) {
        return std::nullopt;
    }
    const auto string = [&](std::string_view key) {
        const core::json::Value* v = json->find(key);
        return std::string(v == nullptr ? "" : v->as_string().value_or(""));
    };
    Seen s{.type = string("type"),
           .seq = 0,
           .sender = string("sender"),
           .body = string("body"),
           .reason = string("reason"),
           .ref = std::nullopt};
    if (const core::json::Value* seq = json->find("seq")) {
        s.seq = seq->as_u64().value_or(0);
    }
    if (const core::json::Value* ref = json->find("ref")) {
        s.ref = ref->as_u64();
    }
    return s;
}

// One user's socket and everything the server has sent it.
class Client {
public:
    Client(WsClient ws, std::string name) : ws_(std::move(ws)), name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<Seen>& seen() const noexcept { return seen_; }

    bool send(const std::string& json) { return ws_.send_text(json); }

    // Reads until a message matching `pred` arrives, keeping everything read.
    template <class Pred>
    std::optional<Seen> wait_for(Pred pred, std::chrono::milliseconds limit = seconds(15)) {
        for (const Seen& s : seen_) {
            if (pred(s)) {
                return s;
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const auto text = ws_.next_text(left);
            if (!text) {
                return std::nullopt;
            }
            auto s = parse_seen(*text);
            if (!s) {
                ADD_FAILURE() << name_ << " got something that is not JSON: " << *text;
                return std::nullopt;
            }
            seen_.push_back(*s);
            if (pred(*s)) {
                return s;
            }
        }
        return std::nullopt;
    }

    std::optional<Seen> message(const std::string& body) {
        return wait_for([&](const Seen& s) { return s.type == "message" && s.body == body; });
    }

    [[nodiscard]] bool ever_saw(const std::string& body) const {
        return std::ranges::any_of(seen_, [&](const Seen& s) { return s.body == body; });
    }

private:
    WsClient ws_;
    std::string name_;
    std::vector<Seen> seen_;
};

struct Node {
    std::string name;
    std::uint16_t port = 0;
    std::uint16_t node_port = 0;
    std::unique_ptr<ChildProcess> process;
};

class ChatClusterTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        room_ = core::RoomId::generate(clock_, random_).to_string();
        // Made up per run: no real secret lives in the repository.
        std::array<std::byte, 32> secret{};
        random_.fill(secret);
        for (const std::byte b : secret) {
            node_secret_ += std::format("{:02x}", std::to_integer<unsigned>(b));
        }
        auto key = devtoken::DevKey::generate();
        ASSERT_TRUE(key);
        const auto jwks = files_.path() / "jwks.json";
        std::ofstream(jwks) << key->public_jwks();
        for (const char* user : {"alice", "bob", "carol"}) {
            tokens_.push_back(*key->mint({.issuer = std::string(kIssuer),
                                          .audience = "askedin-platform",
                                          .subject = user,
                                          .email = {},
                                          .ttl = seconds(600)},
                                         clock_.wall_now()));
        }
        const auto ports = client_ports();
        for (std::size_t i = 0; i < 3; ++i) {
            nodes_.push_back({.name = "chat-" + std::to_string(i + 1),
                              .port = ports[i],
                              .node_port = free_port(),
                              .process = nullptr});
            ASSERT_NE(nodes_.back().port, 0);
            ASSERT_NO_FATAL_FAILURE(start(nodes_.back(), jwks.string()));
        }
        for (const Node& n : nodes_) {
            ASSERT_TRUE(ulw::test::eventually(
                [&] { return ulw::test::http_get(n.port, "/readyz").status == 200; }, seconds(30)))
                << n.process->output();
        }
    }

    void start(Node& node, const std::string& jwks) {
        std::vector<std::string> env{
            "ULW_NODE_ID=" + node.name,
            "ULW_LISTEN_PORT=" + std::to_string(node.port),
            "ULW_NODE_ADDRESS=127.0.0.1:" + std::to_string(node.node_port),
            "ULW_NODE_SECRET=" + node_secret_,
            "ULW_DATABASE_URL=" + db_->conninfo(),
            "ULW_DEV_JWKS_FILE=" + jwks,
            "JWT_ISSUER=" + std::string(kIssuer),
            "ULW_REACTOR=" +
                std::string(GetParam() == net::ReactorKind::IoUring ? "io_uring" : "epoll")};
        for (const char* passed : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"}) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
            if (const char* value = std::getenv(passed)) {
                env.push_back(std::string(passed) + "=" + value);
            }
        }
        node.process = ChildProcess::start({ULW_CHAT_BIN}, env);
        ASSERT_NE(node.process, nullptr);
        ASSERT_TRUE(node.process->wait_for_output(R"("msg":"listening")", seconds(30)))
            << node.process->output();
    }

    std::unique_ptr<Client> connect(const Node& node, std::size_t user) {
        std::string refusal;
        auto ws = WsClient::connect(node.port, "/rt",
                                    "Authorization: Bearer " + tokens_[user] + "\r\n", &refusal);
        if (!ws) {
            ADD_FAILURE() << "upgrade refused: " << refusal;
            return nullptr;
        }
        return std::make_unique<Client>(std::move(*ws),
                                        std::array{"alice", "bob", "carol"}.at(user));
    }

    void join(Client& client) {
        ASSERT_TRUE(client.send(R"({"type":"join","room":")" + room_ + R"("})"));
        const auto joined =
            client.wait_for([](const Seen& s) { return s.type == "joined" || s.type == "error"; });
        ASSERT_TRUE(joined);
        ASSERT_EQ(joined->type, "joined") << joined->reason;
    }

    static std::string send_command(const std::string& room, const std::string& body,
                                    std::uint64_t ref) {
        std::string out = R"({"type":"send","room":")" + room + R"(","ref":)" +
                          std::to_string(ref) + R"(,"body":)";
        core::json::append_string(out, body);
        out += '}';
        return out;
    }

    // Sends until the client gets its own message back: then its node routes to the room's
    // current owner and receives what that owner sequences. Returns the message's seq.
    std::optional<std::uint64_t> send_until_heard(Client& client, const std::string& body) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            const std::uint64_t ref = next_ref_++;
            const std::string text = body + " #" + std::to_string(attempt);
            if (!client.send(send_command(room_, text, ref))) {
                return std::nullopt;
            }
            const auto outcome = client.wait_for([&](const Seen& s) {
                return (s.type == "message" && s.body == text) ||
                       (s.type == "error" && s.ref == ref);
            });
            if (outcome && outcome->type == "message") {
                last_body_[client.name()] = text;
                return outcome->seq;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string owner() const {
        auto conn = db_->session();
        return ulw::test::scalar(conn,
                                 "SELECT owner_node || ' ' || owner_generation "
                                 "FROM room_assignments WHERE room_id = $1::text::uuid",
                                 Params{}.add_text(room_));
    }

    [[nodiscard]] std::string last_seq() const {
        auto conn = db_->session();
        return ulw::test::scalar(conn,
                                 "SELECT last_seq FROM room_state WHERE room_id = $1::text::uuid",
                                 Params{}.add_text(room_));
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    ulw::test::TempDir files_{"ulw-chat-cluster"};
    std::vector<std::string> tokens_;
    std::vector<Node> nodes_;
    std::string room_;
    std::string node_secret_;
    std::uint64_t next_ref_ = 100;
    std::unordered_map<std::string, std::string> last_body_;
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
    ASSERT_TRUE(bob->send(send_command(room_, "hello from chat-2", 1)));
    ASSERT_TRUE(alice->send(send_command(room_, "hello from chat-1", 2)));
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
    ASSERT_TRUE(alice->send(send_command(room_, "stale", 7)));

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
                               R"(","generation":1,"write":"append","rows":0)";
    ASSERT_TRUE(stalled.process->wait_for_output(fenced, seconds(30))) << stalled.process->output();
    const auto refused = alice->wait_for([](const Seen& s) { return s.ref == 7U; });
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->type, "error");
    EXPECT_EQ(refused->reason, "fenced");
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
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatClusterTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
