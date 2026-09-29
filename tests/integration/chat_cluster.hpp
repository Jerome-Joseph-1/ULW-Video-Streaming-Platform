// Three chat_server processes on a scratch Postgres, and clients of theirs: what the M16 to M19
// acceptance (chat_cluster_test.cpp) and the E2EE checkpoint (chat_e2ee_test.cpp) run on.
// ULW_CHAT_CLUSTER_PORTS=9101,9102,9103 pins the client ports (the CI job does); otherwise
// free ones are taken.
#pragma once

#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/auth/base64url.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/eventually.hpp"
#include "support/free_port.hpp"
#include "support/reactor_harness.hpp"
#include "support/temp_dir.hpp"
#include "support/ws_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ulw::test {

inline constexpr std::string_view kIssuer = "https://auth.test.askedin.com";

inline std::vector<std::uint16_t> client_ports() {
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

// A server message, the fields the test looks at. The body is decoded: what the sender sent.
struct Seen {
    std::string type;
    std::uint64_t seq = 0;
    std::string sender;
    std::string id;
    std::string body;
    std::string reason;
    std::optional<std::uint64_t> retry_after_ms;
    // Of a history answer: how many messages came before it.
    std::uint64_t count = 0;
};

inline std::optional<Seen> parse_seen(const std::string& text) {
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
           .id = string("id"),
           .body = infra::auth::decode_base64url(string("body")).value_or("<not base64url>"),
           .reason = string("reason"),
           .retry_after_ms = std::nullopt,
           .count = 0};
    if (const core::json::Value* count = json->find("count")) {
        s.count = count->as_u64().value_or(0);
    }
    if (const core::json::Value* seq = json->find("seq")) {
        s.seq = seq->as_u64().value_or(0);
    }
    if (const core::json::Value* after = json->find("retry_after_ms")) {
        s.retry_after_ms = after->as_u64();
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
    std::optional<Seen> wait_for(Pred pred,
                                 std::chrono::milliseconds limit = std::chrono::seconds(15)) {
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

    template <class Pred> [[nodiscard]] std::size_t count(Pred pred) const {
        return static_cast<std::size_t>(std::ranges::count_if(seen_, pred));
    }

    [[nodiscard]] std::vector<Seen> messages() const {
        std::vector<Seen> out;
        std::ranges::copy_if(seen_, std::back_inserter(out),
                             [](const Seen& s) { return s.type == "message"; });
        return out;
    }

private:
    WsClient ws_;
    std::string name_;
    std::vector<Seen> seen_;
};

// A body as it was sent, as the client encoded it, and as bytea would show it.
inline std::array<std::string, 3> readable_forms(const std::string& text) {
    std::string hex;
    for (const char c : text) {
        hex += std::format("{:02x}", static_cast<unsigned char>(c));
    }
    return {text, infra::auth::encode_base64url(text), hex};
}

struct Node {
    std::string name;
    std::uint16_t port = 0;
    std::uint16_t node_port = 0;
    std::unique_ptr<ChildProcess> process;
};

class ChatCluster : public ::testing::TestWithParam<net::ReactorKind> {
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
        jwks_ = jwks.string();
        for (const char* user : {"alice", "bob", "carol"}) {
            tokens_.push_back(*key->mint({.issuer = std::string(kIssuer),
                                          .audience = "askedin-platform",
                                          .subject = user,
                                          .email = {},
                                          .ttl = std::chrono::seconds(600)},
                                         clock_.wall_now()));
        }
        const auto ports = client_ports();
        for (std::size_t i = 0; i < 3; ++i) {
            nodes_.push_back({.name = "chat-" + std::to_string(i + 1),
                              .port = ports[i],
                              .node_port = free_port(),
                              .process = nullptr});
            ASSERT_NE(nodes_.back().port, 0);
            ASSERT_NO_FATAL_FAILURE(start(nodes_.back(), jwks_));
        }
        ASSERT_NO_FATAL_FAILURE(wait_ready());
    }

    void wait_ready() {
        for (const Node& n : nodes_) {
            ASSERT_TRUE(eventually([&] { return http_get(n.port, "/readyz").status == 200; },
                                   std::chrono::seconds(30)))
                << n.process->output();
        }
    }

    void start(Node& node, const std::string& jwks) {
        std::vector<std::string> env{
            "ULW_NODE_ID=" + node.name,
            "ULW_LISTEN_PORT=" + std::to_string(node.port),
            "ULW_NODE_ADDRESS=127.0.0.1:" + std::to_string(node.node_port),
            "ULW_DEV_LOOPBACK_NODES=1",
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
        ASSERT_TRUE(node.process->wait_for_output(R"("msg":"listening")", std::chrono::seconds(30)))
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

    void join(Client& client, std::optional<std::uint64_t> after = std::nullopt) {
        ASSERT_TRUE(client.send(R"({"type":"join","room":")" + room_ + R"(")" +
                                (after ? R"(,"after":)" + std::to_string(*after) : "") + "}"));
        const auto joined =
            client.wait_for([](const Seen& s) { return s.type == "joined" || s.type == "error"; });
        ASSERT_TRUE(joined);
        ASSERT_EQ(joined->type, "joined") << joined->reason;
    }

    static std::string send_command(const std::string& room, const std::string& body,
                                    const std::string& id) {
        return R"({"type":"send","room":")" + room + R"(","id":")" + id + R"(","body":")" +
               infra::auth::encode_base64url(body) + R"("})";
    }

    static std::string ref(std::uint64_t n) { return "r" + std::to_string(n); }

    // Asks for a page of the room's history with `fields` (`,"after":7`, say) and returns the
    // messages that came before the page's end, as sent; nullopt for an error instead.
    std::optional<std::vector<Seen>> history(Client& client, const std::string& fields) {
        const auto is_end = [](const Seen& s) { return s.type == "history" || s.type == "error"; };
        const std::size_t ends = client.count(is_end);
        const std::size_t from = client.seen().size();
        if (!client.send(R"({"type":"history","room":")" + room_ + R"(")" + fields + "}")) {
            return std::nullopt;
        }
        const auto end = client.wait_for([&](const Seen&) { return client.count(is_end) > ends; });
        if (!end || end->type != "history") {
            ADD_FAILURE() << "history: " << (end ? end->reason : "no answer");
            return std::nullopt;
        }
        std::vector<Seen> page;
        for (std::size_t i = from; i < client.seen().size(); ++i) {
            if (client.seen()[i].type == "message") {
                page.push_back(client.seen()[i]);
            }
        }
        EXPECT_EQ(page.size(), end->count);
        return page;
    }

    // Sends until the client gets its own message back: then its node routes to the room's
    // current owner and receives what that owner sequences. Returns the message's seq.
    std::optional<std::uint64_t> send_until_heard(Client& client, const std::string& body) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            const std::string id = ref(next_ref_++);
            const std::string text = body + " #" + std::to_string(attempt);
            if (!client.send(send_command(room_, text, id))) {
                return std::nullopt;
            }
            const auto outcome = client.wait_for([&](const Seen& s) {
                return (s.type == "message" && s.body == text) || (s.type == "error" && s.id == id);
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
        return scalar(conn,
                      "SELECT owner_node || ' ' || owner_generation "
                      "FROM room_assignments WHERE room_id = $1::text::uuid",
                      infra::postgres::Params{}.add_text(room_));
    }

    [[nodiscard]] std::string last_seq() const {
        auto conn = db_->session();
        return scalar(conn, "SELECT last_seq FROM room_state WHERE room_id = $1::text::uuid",
                      infra::postgres::Params{}.add_text(room_));
    }

    // Every row of every table, as text, but for the bytes of stored bodies: those are the
    // messages themselves, and are checked apart (stored_exactly). Their length stands in.
    [[nodiscard]] std::string database_text() const {
        auto conn = db_->session();
        return scalar(conn, R"sql(
SELECT string_agg(query_to_xml(format('SELECT t::text AS row FROM %I.%I t', table_schema,
                                      table_name), false, false, '')::text, E'\n')
  FROM information_schema.tables
 WHERE table_type = 'BASE TABLE' AND table_schema NOT IN ('pg_catalog', 'information_schema')
   AND table_name <> 'chat_messages')sql") +
               scalar(conn, R"sql(
SELECT string_agg(concat_ws(' ', room_id, seq, sender, msg_key, sent_at, octet_length(body)),
                  E'\n')
  FROM chat_messages)sql");
    }

    // How many of the room's stored messages hold exactly `body`'s bytes.
    [[nodiscard]] std::string stored_exactly(const std::string& body) const {
        auto conn = db_->session();
        return scalar(
            conn, "SELECT count(*) FROM chat_messages WHERE room_id = $1::text::uuid AND body = $2",
            infra::postgres::Params{}.add_text(room_).add_bytea(std::as_bytes(std::span{body})));
    }

    [[nodiscard]] static std::uint64_t metric(const Node& node, std::string_view name) {
        const std::string body = http_get(node.port, "/metrics").body;
        const std::string line = std::string(name) + " ";
        const std::size_t at = body.find(line);
        if (at == std::string::npos || (at > 0 && body[at - 1] != '\n')) {
            ADD_FAILURE() << name << " is not in the metrics";
            return 0;
        }
        const std::size_t start = at + line.size();
        return core::parse_integer<std::uint64_t>(
                   std::string_view(body).substr(start, body.find('\n', start) - start))
            .value_or(0);
    }

    // Section 8.15: bodies are never logged, indexed or stored as anything a grep could read.
    // Each body the room stored is there once, as exactly the bytes sent: that is what the
    // search below covers. Everything else in the database, and every node's log, is searched
    // for each body as it was sent, as the client encoded it, and as bytea would show it.
    void expect_no_plaintext(const std::vector<std::string>& bodies,
                             const std::vector<std::string>& never_stored = {}) const {
        for (const std::string& body : bodies) {
            const bool refused = std::ranges::find(never_stored, body) != never_stored.end();
            EXPECT_EQ(stored_exactly(body), refused ? "0" : "1") << body;
        }
        const std::size_t searched = expect_in_no_row_or_log(bodies);
        if (!HasFailure()) {
            std::cout << "each stored once as the bytes sent; searched " << searched
                      << " bytes of other rows and " << nodes_.size() << " logs for "
                      << bodies.size() << (bodies.size() == 1 ? " body" : " bodies")
                      << " in three forms: none found\n";
        }
    }

    // Every row but the stored bodies, and every node's log, searched for each of `texts` in
    // each of its readable_forms. Returns how many bytes of rows were searched.
    std::size_t expect_in_no_row_or_log(const std::vector<std::string>& texts) const {
        const std::string database = database_text();
        EXPECT_NE(database.find(room_), std::string::npos) << "no rows read";
        for (const std::string& text : texts) {
            for (const std::string& form : readable_forms(text)) {
                EXPECT_EQ(database.find(form), std::string::npos) << "the database holds " << form;
                for (const Node& n : nodes_) {
                    EXPECT_EQ(n.process->output().find(form), std::string::npos)
                        << n.name << " logged " << form;
                }
            }
        }
        return database.size();
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    TempDir files_{"ulw-chat-cluster"};
    std::vector<std::string> tokens_;
    std::vector<Node> nodes_;
    std::string room_;
    std::string node_secret_;
    std::string jwks_;
    std::uint64_t next_ref_ = 100;
    std::unordered_map<std::string, std::string> last_body_;
};

} // namespace ulw::test
