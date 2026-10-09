// The M22 acceptance: end-to-end encryption needs nothing from the chat server. Three devices,
// each an MLS client over the OpenMLS bridge (ADR-0044), form a group and send 1000 application
// messages through three chat_server processes, and each reads every other's in the room's
// order. The room stores the ciphertext exactly as sent, which gzip cannot shrink by more than
// MLS's own framing, and no plaintext is in any node's log or anywhere in the database in any
// readable form. The other half is tools/e2ee_diagnostic_check.sh: neither the chat service nor
// the router changed for it.

#include "infra/e2ee/mls.hpp"

#include "chat_cluster.hpp"
#include "result.hpp"
#include "sync_connection.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <zlib.h>

namespace {

using infra::e2ee::MlsBytes;
using infra::e2ee::MlsClient;
using infra::e2ee::MlsGroup;
using infra::e2ee::MlsReceived;
using infra::postgres::Params;
using std::chrono::seconds;
using ulw::test::Client;
using ulw::test::Seen;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMessages = 1000;

// The bytes of each stored body that are not ciphertext: an MLSMessage holding a PrivateMessage
// (RFC 9420, sections 6 and 6.3), the only form OpenMLS's default wire format policy sends
// application messages in. version 2 + wire_format 2 + group_id (a 1-byte length and the room's
// 36-byte uuid text) 37 + epoch 8 + content_type 1 + authenticated_data (empty: its 1-byte
// length) 1 + encrypted_sender_data's 1-byte length 1 + ciphertext's length 2 (a two-byte varint
// holds 64 to 16383, and these ciphertexts are 596 to 1619 bytes) = 54.
constexpr std::size_t kFramingBytes = 54;
// The rest is AES-GCM output: the encrypted sender data (leaf index, generation and reuse guard,
// 12 bytes, and a 16-byte tag) 28, and the ciphertext of the content (the plaintext with its
// two-byte length, the 64-byte signature with its two-byte length, since a one-byte varint stops
// at 63, no padding) and a 16-byte tag, plaintext + 84. So a body is its plaintext and 166 bytes,
// and the test checks that it is, since the bound below rests on this layout.
constexpr std::size_t kSealedBytes = 28 + 84;

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span{text});
}

std::string text_of(const MlsBytes& bytes) {
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(),
                           [](std::byte b) { return std::to_integer<char>(b); });
    return text;
}

// The size gzip -9 makes of `data`, header and trailer included.
std::size_t gzip_size(std::string_view data) {
    z_stream z{};
    // 15 + 16: a 32 KiB window, as gzip's, in a gzip wrapper rather than zlib's.
    if (deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
        return 0;
    }
    std::string out(deflateBound(&z, data.size()), '\0');
    // zlib's buffers are unsigned char; ZLIB_CONST makes the input const.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    z.next_in = reinterpret_cast<const Bytef*>(data.data());
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    z.avail_out = static_cast<uInt>(out.size());
    const bool done = deflate(&z, Z_FINISH) == Z_STREAM_END;
    const std::size_t size = z.total_out;
    deflateEnd(&z);
    return done ? size : 0;
}

double gzip_ratio(std::string_view data) {
    return static_cast<double>(gzip_size(data)) / static_cast<double>(data.size());
}

// What a person might write, and so as compressible as prose: if any of it were stored in the
// clear, the ratio would show it. Unique by the sender and the number up front, and 512 to 1535
// bytes long (389 is odd, so i * 389 mod 1024 comes round to every length).
std::string plaintext(std::string_view user, std::size_t i) {
    constexpr std::string_view kLine = "see you at the north entrance at half past six; "
                                       "bring both tickets and the spare charger. ";
    std::string text = std::format("{} #{:04}: ", user, i);
    const std::size_t size = 512 + (i * 389) % 1024;
    while (text.size() < size) {
        text += kLine;
    }
    text.resize(size);
    return text;
}

// One person's device: its MLS client and group, and a connection to every node. The send rate
// limit is per user and node (ServiceLimits: a burst of 10, then 2 a second), so a device
// spreads its sends over three buckets rather than one.
struct Device {
    std::string user;
    MlsClient mls;
    std::optional<MlsGroup> group;
    std::vector<std::unique_ptr<Client>> links;
    // When each link's bucket next has a token, as its last refusal said.
    std::vector<Clock::time_point> refilled;
    std::size_t next_link = 0;
};

struct Sent {
    std::string user;
    std::string plaintext;
    std::string ciphertext;
};

class ChatE2eeTest : public ulw::test::ChatCluster {
protected:
    // Sends `ciphertext` through the next link with a token, and returns the seq it got. A
    // refusal says when to come back (retry_after_ms); with every link refused, the device waits
    // for the soonest on its socket, reading deliveries meanwhile.
    std::optional<std::uint64_t> send_paced(Device& d, const std::string& ciphertext,
                                            const std::string& id) {
        // Three links, each refused at most once or twice per token: far fewer tries than this.
        for (int attempt = 0; attempt < 64; ++attempt) {
            // The link whose bucket refills first, unless the next one in turn has a token too.
            auto pick =
                static_cast<std::size_t>(std::ranges::min_element(d.refilled) - d.refilled.begin());
            // The socket's wait counts whole milliseconds, rounded down, so it can end short.
            while (Clock::now() < d.refilled[pick]) {
                (void)d.links[0]->wait_for(
                    [](const Seen&) { return false; },
                    std::chrono::ceil<std::chrono::milliseconds>(d.refilled[pick] - Clock::now()));
            }
            const auto now = Clock::now();
            for (std::size_t k = 0; k < d.links.size(); ++k) {
                const std::size_t j = (d.next_link + k) % d.links.size();
                if (d.refilled[j] <= now) {
                    pick = j;
                    break;
                }
            }
            d.next_link = (pick + 1) % d.links.size();
            Client& link = *d.links[pick];
            // An id per try: a refused try was never sequenced, and its answer must not be
            // mistaken for the next one's.
            const std::string try_id = std::format("{}-{}", id, attempt);
            if (!link.send(send_command(room_, ciphertext, try_id))) {
                ADD_FAILURE() << d.user << " could not send " << try_id;
                return std::nullopt;
            }
            const auto answer = link.wait_for([&](const Seen& s) {
                return (s.type == "sent" || s.type == "error") && s.id == try_id;
            });
            if (!answer) {
                ADD_FAILURE() << d.user << " got no answer to " << try_id;
                return std::nullopt;
            }
            if (answer->type == "sent") {
                return answer->seq;
            }
            if (answer->reason != "rate_limited" || !answer->retry_after_ms) {
                ADD_FAILURE() << try_id << " refused: " << answer->reason;
                return std::nullopt;
            }
            d.refilled[pick] = Clock::now() + std::chrono::milliseconds(*answer->retry_after_ms);
        }
        ADD_FAILURE() << id << " never accepted";
        return std::nullopt;
    }

    // The room's stored bodies, by seq.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::string>> stored_bodies() const {
        auto conn = db_->session();
        const auto rows = conn.exec(
            "SELECT seq, body FROM chat_messages WHERE room_id = $1::text::uuid ORDER BY seq",
            Params{}.add_text(room_));
        std::vector<std::pair<std::uint64_t, std::string>> out;
        if (!rows) {
            ADD_FAILURE() << "reading the stored bodies failed";
            return out;
        }
        for (int row = 0; row < rows->rows(); ++row) {
            const auto seq = infra::postgres::parse_uint64(rows->get(row, 0).value_or(""));
            const auto body = infra::postgres::parse_bytea(rows->get(row, 1).value_or(""));
            if (!seq || !body) {
                ADD_FAILURE() << "row " << row << " is not a seq and a bytea";
                return out;
            }
            out.emplace_back(*seq, text_of(*body));
        }
        return out;
    }
};

TEST_P(ChatE2eeTest, AThousandMlsMessagesAreStoredAsCiphertextAndReadableNowhere) {
    std::vector<Device> devices;
    for (std::size_t user = 0; user < 3; ++user) {
        const std::string name = std::array{"alice", "bob", "carol"}.at(user);
        auto mls = MlsClient::create(bytes_of(name));
        ASSERT_TRUE(mls);
        Device d{.user = name,
                 .mls = std::move(*mls),
                 .group = std::nullopt,
                 .links = {},
                 .refilled = {},
                 .next_link = 0};
        for (const ulw::test::Node& node : nodes_) {
            d.links.push_back(connect(node, user));
            ASSERT_TRUE(d.links.back());
            ASSERT_NO_FATAL_FAILURE(join(*d.links.back()));
            d.refilled.push_back(Clock::now());
        }
        devices.push_back(std::move(d));
    }

    // Key packages and the welcome reach devices through the directory's delivery service, as
    // mls_directory_test does; here they are handed over directly, and the room carries what
    // rooms carry, the group's messages.
    Device& alice = devices[0];
    auto created = alice.mls.create_group(bytes_of(room_));
    ASSERT_TRUE(created);
    alice.group = std::move(*created);
    std::vector<MlsBytes> packages;
    for (std::size_t k = 1; k < devices.size(); ++k) {
        auto package = devices[k].mls.key_package();
        ASSERT_TRUE(package);
        packages.push_back(std::move(*package));
    }
    const auto added = alice.group->add(packages);
    ASSERT_TRUE(added);
    ASSERT_TRUE(alice.group->merge_pending_commit());
    for (std::size_t k = 1; k < devices.size(); ++k) {
        auto joined = devices[k].mls.join(added->welcome);
        ASSERT_TRUE(joined);
        devices[k].group = std::move(*joined);
    }
    for (const Device& d : devices) {
        ASSERT_EQ(d.group->member_count().value_or(0), 3U) << d.user;
    }

    // Each device sends in turn, one message acknowledged before it encrypts the next, so the
    // room's order of a sender's messages is the order of its MLS generations.
    const auto started = Clock::now();
    std::unordered_map<std::uint64_t, Sent> by_seq;
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < kMessages; ++i) {
        Device& d = devices[i % devices.size()];
        Sent sent{.user = d.user, .plaintext = plaintext(d.user, i), .ciphertext = {}};
        const auto ciphertext = d.group->encrypt(bytes_of(sent.plaintext));
        ASSERT_TRUE(ciphertext);
        sent.ciphertext = text_of(*ciphertext);
        const auto seq = send_paced(d, sent.ciphertext, std::format("{}-{}", d.user, i));
        ASSERT_TRUE(seq);
        ASSERT_TRUE(by_seq.emplace(*seq, std::move(sent)).second) << "seq " << *seq << " twice";
        last = std::max(last, *seq);
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    ASSERT_EQ(last, kMessages) << "seqs 1 to 1000, one per message";

    // Every device hears every message, as the bytes sent, and decrypts all but its own.
    for (Device& d : devices) {
        Client& home = *d.links[0];
        ASSERT_TRUE(home.wait_for([&](const Seen& s) {
            return s.type == "message" && s.seq == last;
        })) << d.user;
        const std::vector<Seen> heard = home.messages();
        ASSERT_EQ(heard.size(), kMessages) << d.user;
        std::size_t decrypted = 0;
        for (std::size_t k = 0; k < heard.size(); ++k) {
            const Seen& m = heard[k];
            ASSERT_EQ(m.seq, k + 1) << d.user;
            const Sent& sent = by_seq.at(m.seq);
            ASSERT_EQ(m.sender, sent.user);
            ASSERT_EQ(m.body, sent.ciphertext) << "seq " << m.seq;
            if (m.sender == d.user) {
                continue;
            }
            const auto read = d.group->process(bytes_of(m.body));
            ASSERT_TRUE(read) << d.user << " seq " << m.seq << ": " << to_string(read.error());
            ASSERT_EQ(read->kind, MlsReceived::Application);
            ASSERT_EQ(text_of(read->plaintext), sent.plaintext);
            ++decrypted;
        }
        const auto own = std::ranges::count(by_seq | std::views::values, d.user, &Sent::user);
        EXPECT_EQ(decrypted, kMessages - static_cast<std::size_t>(own)) << d.user;
    }

    // What the room stored is those ciphertexts, byte for byte, and nothing gzip can shrink.
    const auto stored = stored_bodies();
    ASSERT_EQ(stored.size(), kMessages);
    std::string bodies;
    std::string plaintexts;
    std::string encoded;
    for (const auto& [seq, body] : stored) {
        const Sent& sent = by_seq.at(seq);
        ASSERT_EQ(body, sent.ciphertext) << "seq " << seq;
        EXPECT_EQ(body.size(), sent.plaintext.size() + kFramingBytes + kSealedBytes)
            << "seq " << seq;
        bodies += body;
        plaintexts += sent.plaintext;
        encoded += infra::auth::encode_base64url(body);
    }
    // gzip cannot shrink AES-GCM output at all, so it saves at most kFramingBytes a message, and
    // the ratio it reaches is at least (total - 54 n) / total: about 0.95 at these sizes. A body
    // stored as anything but ciphertext falls well short of that: the plaintexts, as repetitive as
    // these are, compress to a small fraction, and the ciphertexts' own base64url, what a server
    // that stored the client's text would hold, to about three quarters (6 bits of every 8).
    const double ratio = gzip_ratio(bodies);
    const double least = static_cast<double>(bodies.size() - kMessages * kFramingBytes) /
                         static_cast<double>(bodies.size());
    EXPECT_GE(ratio, least);
    EXPECT_LT(gzip_ratio(plaintexts), least) << "the check could not tell plaintext apart";
    EXPECT_LT(gzip_ratio(encoded), least) << "the check could not tell base64url apart";
    std::cout << std::format(
        "{} messages from 3 devices through 3 nodes in {} ms; {} bytes stored, "
        "gzip -9 keeps {:.4f} of them (framing allows no less than {:.4f}); "
        "the plaintexts would keep {:.4f}, the ciphertexts' base64url {:.4f}\n",
        kMessages, took.count(), bodies.size(), ratio, least, gzip_ratio(plaintexts),
        gzip_ratio(encoded));

    // Every node's log, read to its end, and every row, bodies included, hold no plaintext.
    for (ulw::test::Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (ulw::test::Node& n : nodes_) {
        EXPECT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }
    std::vector<std::string> plaintext_list;
    for (const auto& [seq, s] : by_seq) {
        plaintext_list.push_back(s.plaintext);
        for (const std::string& form : ulw::test::readable_forms(s.plaintext)) {
            EXPECT_EQ(bodies.find(form), std::string::npos) << "a stored body holds " << form;
        }
    }
    const std::size_t searched = expect_in_no_row_or_log(plaintext_list);
    std::size_t logged = 0;
    for (const ulw::test::Node& n : nodes_) {
        logged += n.process->output().size();
    }
    if (!HasFailure()) {
        std::cout << "searched " << bodies.size() << " bytes of bodies, " << searched
                  << " bytes of other rows and " << logged << " bytes of " << nodes_.size()
                  << " logs for " << plaintext_list.size()
                  << " plaintexts in three forms: none found\n";
    }
}

// ADR-0101: each device of a user is its own member, found through the server's key directory.
// Alice has a phone and a laptop, bob one device, each on its own node. Every device publishes
// key packages it made itself; alice's phone lists the devices of everyone in the room, claims a
// package of each device the group lacks, and adds them all in one commit; the welcome reaches
// the others through the room. Then every device reads every other's messages.
TEST_P(ChatE2eeTest, EveryDeviceOfEveryMemberJoinsThroughTheKeyDirectory) {
    struct Member {
        std::string user;
        std::string device;
        MlsClient mls;
        std::optional<MlsGroup> group;
        std::unique_ptr<Client> link;
    };
    const auto ask = [](Client& client, const std::string& command, std::string_view answer) {
        const std::size_t from = client.seen().size();
        EXPECT_TRUE(client.send(command));
        const auto at = client.wait_from(
            from, [&](const Seen& s) { return s.type == answer || s.type == "error"; });
        return at ? std::optional<Seen>(client.seen()[*at]) : std::nullopt;
    };
    std::vector<Member> members;
    const std::array<std::pair<std::size_t, std::size_t>, 3> who{
        std::pair<std::size_t, std::size_t>{0, 0}, {0, 1}, {1, 2}};
    for (const auto& [user, node] : who) {
        const std::string name = std::array{"alice", "bob"}.at(user);
        const std::string device = core::DeviceId::generate(clock_, random_).to_string();
        // The credential names the user and the device as the directory knows it.
        auto mls = MlsClient::create(bytes_of(std::format("{}/{}", name, device)));
        ASSERT_TRUE(mls);
        auto link = connect(nodes_[node], user);
        ASSERT_TRUE(link);
        ASSERT_NO_FATAL_FAILURE(join(*link));
        members.push_back({.user = name,
                           .device = device,
                           .mls = std::move(*mls),
                           .group = std::nullopt,
                           .link = std::move(link)});
    }
    for (Member& m : members) {
        const auto registered =
            ask(*m.link, R"({"type":"register_device","device":")" + m.device + R"("})",
                "device_registered");
        ASSERT_TRUE(registered);
        ASSERT_EQ(registered->type, "device_registered") << registered->raw;
        std::string list;
        for (int i = 0; i < 3; ++i) {
            auto package = m.mls.key_package();
            ASSERT_TRUE(package);
            list += (list.empty() ? "\"" : ",\"") +
                    infra::auth::encode_base64url(std::string_view(text_of(*package))) + "\"";
        }
        const auto published = ask(*m.link,
                                   R"({"type":"publish_key_packages","device":")" + m.device +
                                       R"(","key_packages":[)" + list + "]}",
                                   "key_packages_published");
        ASSERT_TRUE(published);
        ASSERT_NE(published->raw.find(R"("key_packages":3)"), std::string::npos) << published->raw;
    }

    // Alice's phone starts the group, and adds every other device of the room's members.
    Member& phone = members[0];
    auto created = phone.mls.create_group(bytes_of(room_));
    ASSERT_TRUE(created);
    phone.group = std::move(*created);
    std::vector<MlsBytes> packages;
    std::set<std::string> claimed_devices;
    for (const std::string user : {"alice", "bob"}) {
        const auto listed =
            ask(*phone.link, R"({"type":"devices","user":")" + user + R"("})", "devices");
        ASSERT_TRUE(listed);
        ASSERT_EQ(listed->type, "devices") << listed->raw;
        std::string wanted;
        for (const Member& m : members) {
            if (m.user == user && m.device != phone.device) {
                ASSERT_NE(listed->raw.find(m.device), std::string::npos) << listed->raw;
                wanted += (wanted.empty() ? "\"" : ",\"") + m.device + "\"";
            }
        }
        const auto claim =
            ask(*phone.link,
                std::format(R"({{"type":"claim_key_packages","user":"{}","devices":[{}]}})", user,
                            wanted),
                "key_packages");
        ASSERT_TRUE(claim);
        ASSERT_EQ(claim->type, "key_packages") << claim->raw;
        const auto json = core::json::parse(claim->raw);
        ASSERT_TRUE(json);
        for (const core::json::Value& item : *json->find("key_packages")->as_array()) {
            claimed_devices.insert(std::string(item.find("device")->as_string().value_or("")));
            const auto bytes =
                infra::auth::decode_base64url(item.find("key_package")->as_string().value_or(""));
            ASSERT_TRUE(bytes);
            const auto view = bytes_of(*bytes);
            packages.emplace_back(view.begin(), view.end());
        }
    }
    ASSERT_EQ(claimed_devices, (std::set<std::string>{members[1].device, members[2].device}));
    const auto added = phone.group->add(packages);
    ASSERT_TRUE(added) << to_string(added.error());
    ASSERT_TRUE(phone.link->send(send_command(room_, text_of(added->commit), "commit-0")));
    ASSERT_TRUE(phone.link->wait_for(
        [](const Seen& s) { return s.type == "message" && s.id == "commit-0"; }));
    ASSERT_TRUE(phone.group->merge_pending_commit());
    ASSERT_TRUE(phone.link->send(send_command(room_, text_of(added->welcome), "welcome-0")));
    for (std::size_t k = 1; k < members.size(); ++k) {
        Member& m = members[k];
        const auto welcome = m.link->wait_for(
            [](const Seen& s) { return s.type == "message" && s.id == "welcome-0"; });
        ASSERT_TRUE(welcome) << m.user;
        auto joined = m.mls.join(bytes_of(welcome->body));
        ASSERT_TRUE(joined) << m.user << ": " << to_string(joined.error());
        m.group = std::move(*joined);
    }
    for (const Member& m : members) {
        EXPECT_EQ(m.group->member_count().value_or(0), 3U) << m.user;
    }

    // Each device says something; every other one reads it, the same user's other device too.
    for (std::size_t k = 0; k < members.size(); ++k) {
        Member& m = members[k];
        const std::string text = std::format("from {} device {}", m.user, k);
        const auto sealed = m.group->encrypt(bytes_of(text));
        ASSERT_TRUE(sealed);
        const std::string id = std::format("app-{}", k);
        ASSERT_TRUE(m.link->send(send_command(room_, text_of(*sealed), id)));
        for (std::size_t j = 0; j < members.size(); ++j) {
            if (j == k) {
                continue;
            }
            Member& reader = members[j];
            const auto heard = reader.link->wait_for(
                [&](const Seen& s) { return s.type == "message" && s.id == id; });
            ASSERT_TRUE(heard) << reader.user << " " << j;
            const auto read = reader.group->process(bytes_of(heard->body));
            ASSERT_TRUE(read) << to_string(read.error());
            EXPECT_EQ(text_of(read->plaintext), text);
        }
    }

    // Exactly the two packages added were spent, and none of the phone's.
    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(conn, "SELECT count(*) FROM key_packages"), "7");
    EXPECT_EQ(ulw::test::scalar(conn,
                                "SELECT count(*) FROM key_packages WHERE device_id = "
                                "$1::text::uuid",
                                Params{}.add_text(phone.device)),
              "3");
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatE2eeTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
