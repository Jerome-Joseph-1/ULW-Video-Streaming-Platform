// The key directory as chat_server serves it (ADR-0102), on the three-node cluster: a device
// publishes its key packages through one node, and other users claim them through the others,
// each package once however many claims race for it.

#include "chat_cluster.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <future>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using ulw::test::Client;
using ulw::test::Seen;

// A recognisable package of 300 bytes: the server never reads one, so any bytes will do.
std::string package_text(std::size_t n) {
    std::string text = std::format("key-package-{:04}-", n);
    text.resize(300, static_cast<char>('a' + (n % 26)));
    return text;
}

class ChatKeyDirectoryTest : public ulw::test::ChatCluster {
protected:
    std::string new_device() { return core::DeviceId::generate(clock_, random_).to_string(); }

    // Registers `device` for the client's user and publishes `count` packages, numbered from
    // `first`, and a last-resort one when asked.
    static void stock(Client& client, const std::string& device, std::size_t first,
                      std::size_t count,
                      const std::optional<std::string>& last_resort = std::nullopt) {
        const auto registered =
            ask(client, R"({"type":"register_device","device":")" + device + R"("})",
                "device_registered");
        ASSERT_TRUE(registered);
        ASSERT_EQ(registered->type, "device_registered") << registered->raw;
        std::string list;
        for (std::size_t i = first; i < first + count; ++i) {
            list += (list.empty() ? "\"" : ",\"") + infra::auth::encode_base64url(package_text(i)) +
                    "\"";
        }
        std::string command = R"({"type":"publish_key_packages","device":")" + device + R"(")";
        if (count > 0) {
            command += R"(,"key_packages":[)" + list + "]";
        }
        if (last_resort) {
            command += R"(,"last_resort":")" + infra::auth::encode_base64url(*last_resort) + R"(")";
        }
        const auto published = ask(client, command + "}", "key_packages_published");
        ASSERT_TRUE(published);
        ASSERT_EQ(published->type, "key_packages_published") << published->raw;
    }

    // Each answer's packages, as text: [device, package, last_resort].
    struct Got {
        std::string device;
        std::string package;
        bool last_resort = false;
    };
    static std::vector<Got> packages_of(const Seen& answer) {
        std::vector<Got> out;
        const auto json = core::json::parse(answer.raw);
        const core::json::Value* list = json ? json->find("key_packages") : nullptr;
        if (list == nullptr || list->as_array() == nullptr) {
            return out;
        }
        for (const core::json::Value& item : *list->as_array()) {
            out.push_back({.device = std::string(item.find("device")->as_string().value_or("")),
                           .package = infra::auth::decode_base64url(
                                          item.find("key_package")->as_string().value_or(""))
                                          .value_or(""),
                           .last_resort = item.find("last_resort")->as_bool().value_or(false)});
        }
        return out;
    }
    static std::size_t listed(const Seen& answer, std::string_view key) {
        const auto json = core::json::parse(answer.raw);
        const core::json::Value* list = json ? json->find(key) : nullptr;
        return list == nullptr || list->as_array() == nullptr ? 0 : list->as_array()->size();
    }

    // Sends `total` claims of `target`'s devices on `client`, at most `window` waiting at once,
    // and returns every answer.
    static std::vector<Seen> claim_many(Client& client, const std::string& target,
                                        std::size_t total, std::size_t window,
                                        const std::string& tag) {
        std::vector<Seen> answers;
        std::size_t sent = 0;
        std::size_t from = client.seen().size();
        while (answers.size() < total) {
            while (sent < total && sent - answers.size() < window) {
                EXPECT_TRUE(client.send(
                    std::format(R"({{"type":"claim_key_packages","user":"{}","id":"{}{}"}})",
                                target, tag, sent)));
                ++sent;
            }
            const auto at = client.wait_from(from, [&](const Seen& s) {
                return (s.type == "key_packages" || s.type == "error") && s.id.starts_with(tag);
            });
            if (!at) {
                ADD_FAILURE() << client.name() << " waited in vain for a claim's answer";
                return answers;
            }
            answers.push_back(client.seen()[*at]);
            from = *at + 1;
        }
        return answers;
    }
};

TEST_P(ChatKeyDirectoryTest, PackagesPublishedThroughOneNodeAreClaimedThroughOthersOnceEach) {
    constexpr std::size_t kStock = 40;
    constexpr std::size_t kClaimsEach = 30;
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && bob && carol);
    const std::string phone = new_device();
    ASSERT_NO_FATAL_FAILURE(stock(*alice, phone, 0, kStock));

    // Two claimers on two nodes race for the same device's packages, at once, eight claims in
    // flight each, more claims between them than there are packages.
    auto carols = std::async(std::launch::async,
                             [&] { return claim_many(*carol, "alice", kClaimsEach, 8, "c"); });
    std::vector<Seen> answers = claim_many(*bob, "alice", kClaimsEach, 8, "b");
    const std::vector<Seen> carol_answers = carols.get();
    answers.insert(answers.end(), carol_answers.begin(), carol_answers.end());
    ASSERT_EQ(answers.size(), 2 * kClaimsEach);

    std::set<std::string> handed_out;
    std::size_t exhausted = 0;
    for (const Seen& answer : answers) {
        ASSERT_EQ(answer.type, "key_packages") << answer.raw;
        for (const Got& got : packages_of(answer)) {
            EXPECT_EQ(got.device, phone);
            EXPECT_FALSE(got.last_resort);
            EXPECT_TRUE(handed_out.insert(got.package).second) << "handed out twice";
        }
        exhausted += listed(answer, "exhausted");
    }
    EXPECT_EQ(handed_out.size(), kStock);
    EXPECT_EQ(exhausted, (2 * kClaimsEach) - kStock);
    for (std::size_t i = 0; i < kStock; ++i) {
        EXPECT_TRUE(handed_out.contains(package_text(i))) << i;
    }
    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(conn, "SELECT count(*) FROM key_packages"), "0");

    EXPECT_EQ(metric(nodes_[1], R"(e2ee_key_packages_claimed_total{kind="single_use"})") +
                  metric(nodes_[2], R"(e2ee_key_packages_claimed_total{kind="single_use"})"),
              kStock);
    EXPECT_EQ(metric(nodes_[1], "e2ee_claims_exhausted_total") +
                  metric(nodes_[2], "e2ee_claims_exhausted_total"),
              (2 * kClaimsEach) - kStock);
}

TEST_P(ChatKeyDirectoryTest, EveryDeviceOfAUserIsClaimedAndTheLastResortKeepsThemInvitable) {
    auto alice_phone = connect(nodes_[0], 0);
    auto alice_laptop = connect(nodes_[1], 0);
    auto bob = connect(nodes_[2], 1);
    ASSERT_TRUE(alice_phone && alice_laptop && bob);
    const std::string phone = new_device();
    const std::string laptop = new_device();
    ASSERT_NO_FATAL_FAILURE(stock(*alice_phone, phone, 0, 1, "phone last resort"));
    ASSERT_NO_FATAL_FAILURE(stock(*alice_laptop, laptop, 100, 1, "laptop last resort"));

    const auto listed_devices = ask(*bob, R"({"type":"devices","user":"alice"})", "devices");
    ASSERT_TRUE(listed_devices);
    ASSERT_EQ(listed_devices->type, "devices") << listed_devices->raw;
    EXPECT_EQ(listed(*listed_devices, "devices"), 2U);
    EXPECT_EQ(listed_devices->raw.find("key_packages"), std::string::npos)
        << "another user's supply is not shown";

    const auto first = ask(*bob, R"({"type":"claim_key_packages","user":"alice"})", "key_packages");
    ASSERT_TRUE(first);
    std::map<std::string, Got> by_device;
    for (const Got& got : packages_of(*first)) {
        by_device[got.device] = got;
    }
    ASSERT_EQ(by_device.size(), 2U) << first->raw;
    EXPECT_EQ(by_device[phone].package, package_text(0));
    EXPECT_EQ(by_device[laptop].package, package_text(100));
    EXPECT_FALSE(by_device[phone].last_resort);

    // Out of single-use packages, both devices are still invitable through their last resort.
    const auto second =
        ask(*bob, R"({"type":"claim_key_packages","user":"alice","devices":[")" + phone + R"("]})",
            "key_packages");
    ASSERT_TRUE(second);
    const auto again = packages_of(*second);
    ASSERT_EQ(again.size(), 1U) << second->raw;
    EXPECT_EQ(again[0].package, "phone last resort");
    EXPECT_TRUE(again[0].last_resort);
    // The phone's connection, on the node that served nothing of this, learns on its next
    // registration that its last resort was used.
    const auto status =
        ask(*alice_phone, R"({"type":"register_device","device":")" + phone + R"("})",
            "device_registered");
    ASSERT_TRUE(status);
    EXPECT_NE(status->raw.find(R"("key_packages":0,"last_resort":"used")"), std::string::npos)
        << status->raw;

    // Retired, the laptop is gone for claimers, and its packages with it.
    const auto retired =
        ask(*alice_laptop, R"({"type":"retire_device","device":")" + laptop + R"("})",
            "device_retired");
    ASSERT_TRUE(retired);
    ASSERT_EQ(retired->type, "device_retired") << retired->raw;
    const auto after =
        ask(*bob, R"({"type":"claim_key_packages","user":"alice","devices":[")" + laptop + R"("]})",
            "key_packages");
    ASSERT_TRUE(after);
    EXPECT_EQ(listed(*after, "gone"), 1U) << after->raw;
    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(conn,
                                "SELECT count(*) FROM last_resort_key_packages "
                                "WHERE device_id = $1::text::uuid",
                                infra::postgres::Params{}.add_text(laptop)),
              "0");
}

TEST_P(ChatKeyDirectoryTest, OnlyAUserWhoSharesAChatMayClaimAndOnlyTheOwnerMayPublish) {
    auto alice = connect(nodes_[0], 0);
    // dave is in no chat with alice.
    auto dave = connect(nodes_[1], 3);
    ASSERT_TRUE(alice && dave);
    const std::string phone = new_device();
    ASSERT_NO_FATAL_FAILURE(stock(*alice, phone, 0, 2));

    const auto claim =
        ask(*dave, R"({"type":"claim_key_packages","user":"alice","id":"d1"})", "key_packages");
    ASSERT_TRUE(claim);
    EXPECT_EQ(claim->type, "error");
    EXPECT_EQ(claim->reason, "not_shared");
    EXPECT_EQ(claim->id, "d1");
    const auto list = ask(*dave, R"({"type":"devices","user":"alice"})", "devices");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->reason, "not_shared");

    const auto foreign =
        ask(*dave,
            R"({"type":"publish_key_packages","device":")" + phone + R"(","key_packages":[")" +
                infra::auth::encode_base64url(std::string("forged")) + R"("]})",
            "key_packages_published");
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->reason, "unknown_device");

    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(conn, "SELECT count(*) FROM key_packages"), "2");
    EXPECT_EQ(metric(nodes_[1], R"(e2ee_refusals_total{reason="not_shared"})"), 2U);
    EXPECT_EQ(metric(nodes_[1], R"(e2ee_refusals_total{reason="unknown_device"})"), 1U);
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatKeyDirectoryTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
