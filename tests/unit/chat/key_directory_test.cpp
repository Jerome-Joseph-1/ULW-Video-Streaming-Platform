#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/e2ee/memory_directory.hpp"
#include "net/reactor_factory.hpp"

#include "key_directory.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::ports::KeyPackageBytes;

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

KeyPackageBytes package(std::uint8_t seed, std::size_t size = 300) {
    KeyPackageBytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((seed + i) & 0xFFU);
    }
    return out;
}

std::string encoded(const KeyPackageBytes& bytes) {
    std::string out;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    infra::auth::append_base64url(
        out, std::span{reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return out;
}

// Who shares a chat with whom, answered on a later loop iteration as the store's are.
class FakeAccess final : public chat::IPresenceAccess, public net::ITimerHandler {
public:
    explicit FakeAccess(net::IReactor& reactor) : reactor_(reactor) {}
    ~FakeAccess() override { reactor_.cancel_timer(timer_); }
    FakeAccess(const FakeAccess&) = delete;
    FakeAccess& operator=(const FakeAccess&) = delete;
    FakeAccess(FakeAccess&&) = delete;
    FakeAccess& operator=(FakeAccess&&) = delete;

    void shared_with(const core::UserId& asker, std::vector<core::UserId> others,
                     core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        ++asked;
        std::vector<core::UserId> shared;
        for (const core::UserId& other : others) {
            if (pairs.contains({asker.view().data(), other.view().data()}) ||
                pairs.contains({other.view().data(), asker.view().data()})) {
                shared.push_back(other);
            }
        }
        core::ports::MessageResult<std::vector<core::UserId>> answer = std::move(shared);
        if (failing) {
            answer = std::unexpected(core::ports::MessageStoreError::Unavailable);
        }
        pending_.emplace_back([done = std::move(done), answer = std::move(answer)]() mutable {
            done(std::move(answer));
        });
        if (timer_ == net::TimerId{}) {
            timer_ = reactor_.arm_timer(core::Millis{0}, *this);
        }
    }

    void on_timeout() noexcept override {
        timer_ = {};
        auto batch = std::move(pending_);
        pending_.clear();
        for (auto& fn : batch) {
            fn();
        }
    }

    std::set<std::pair<std::string, std::string>> pairs;
    bool failing = false;
    int asked = 0;

private:
    net::IReactor& reactor_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void()>> pending_;
};

class FakeClient final : public chat::IClient {
public:
    bool push(std::string_view text) noexcept override {
        frames.emplace_back(text);
        return true;
    }
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override { return 0; }
    void allocation_failed() noexcept override { ++allocation_failures; }

    std::vector<std::string> frames;
    int allocation_failures = 0;
};

class KeyDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, clock_, 256);
        ASSERT_TRUE(r) << r.error();
        reactor_ = std::move(r->reactor);
        store_ = std::make_unique<infra::e2ee::MemoryDirectory>(*reactor_);
        access_ = std::make_unique<FakeAccess>(*reactor_);
        access_->pairs.insert({"alice", "bob"});
        make(chat::DirectoryLimits{});
    }

    void TearDown() override {
        directory_.reset();
        access_.reset();
        store_.reset();
    }

    void make(chat::DirectoryLimits limits, bool configured = true) {
        directory_ = std::make_unique<chat::KeyDirectory>(configured ? store_.get() : nullptr,
                                                          configured ? store_.get() : nullptr,
                                                          *access_, clock_, limits);
    }

    chat::DirectoryClientId attach(FakeClient& client, std::string_view name) {
        return directory_->attach(client, user(name));
    }

    // Sends one command and returns the frame it is answered with.
    std::optional<core::json::Value> ask(FakeClient& client, chat::DirectoryClientId id,
                                         const std::string& text) {
        const std::size_t from = client.frames.size();
        send(id, text);
        const bool answered = ulw::test::pump_until(*reactor_, [&] {
            return std::ranges::any_of(client.frames.begin() + static_cast<std::ptrdiff_t>(from),
                                       client.frames.end(), [](const std::string& f) {
                                           return f.find(R"("type":"replenish")") ==
                                                  std::string::npos;
                                       });
        });
        if (!answered) {
            ADD_FAILURE() << "no answer to " << text;
            return std::nullopt;
        }
        for (std::size_t i = from; i < client.frames.size(); ++i) {
            if (client.frames[i].find(R"("type":"replenish")") == std::string::npos) {
                auto parsed = core::json::parse(client.frames[i]);
                return parsed ? std::optional<core::json::Value>(std::move(*parsed)) : std::nullopt;
            }
        }
        return std::nullopt;
    }

    void send(chat::DirectoryClientId id, const std::string& text) {
        auto parsed = chat::parse_command(text);
        ASSERT_TRUE(parsed) << text << ": " << chat::reason(parsed.error());
        auto* command = std::get_if<chat::DirectoryCommand>(&*parsed);
        ASSERT_NE(command, nullptr) << text;
        directory_->command(id, std::move(*command));
    }

    static std::string type_of(const std::optional<core::json::Value>& v) {
        if (!v) {
            return "";
        }
        const core::json::Value* t = v->find("type");
        return std::string(t == nullptr ? "" : t->as_string().value_or(""));
    }
    static std::string field(const std::optional<core::json::Value>& v, std::string_view key) {
        if (!v) {
            return "";
        }
        const core::json::Value* t = v->find(key);
        return std::string(t == nullptr ? "" : t->as_string().value_or(""));
    }
    static std::uint64_t number(const std::optional<core::json::Value>& v, std::string_view key) {
        const core::json::Value* t = v ? v->find(key) : nullptr;
        return t == nullptr ? 0 : t->as_u64().value_or(0);
    }

    std::string new_device() { return core::DeviceId::generate(clock_, random_).to_string(); }

    void enrol(FakeClient& client, chat::DirectoryClientId id, const std::string& device,
               const std::vector<KeyPackageBytes>& packages,
               std::optional<KeyPackageBytes> last_resort = std::nullopt) {
        const auto registered =
            ask(client, id, R"({"type":"register_device","device":")" + device + R"("})");
        ASSERT_EQ(type_of(registered), "device_registered") << client.frames.back();
        std::string list;
        for (const KeyPackageBytes& p : packages) {
            list += (list.empty() ? "\"" : ",\"") + encoded(p) + "\"";
        }
        std::string text = R"({"type":"publish_key_packages","device":")" + device + R"(")";
        if (!packages.empty()) {
            text += R"(,"key_packages":[)" + list + "]";
        }
        if (last_resort) {
            text += R"(,"last_resort":")" + encoded(*last_resort) + R"(")";
        }
        const auto published = ask(client, id, text + "}");
        ASSERT_EQ(type_of(published), "key_packages_published") << client.frames.back();
        EXPECT_EQ(number(published, "key_packages"), packages.size());
        EXPECT_EQ(field(published, "last_resort"), last_resort ? "fresh" : "none");
    }

    // The packages of a key_packages answer, by device: their bytes and whether last-resort.
    static std::vector<std::pair<std::string, std::pair<KeyPackageBytes, bool>>>
    claimed(const std::optional<core::json::Value>& answer) {
        std::vector<std::pair<std::string, std::pair<KeyPackageBytes, bool>>> out;
        const core::json::Value* list = answer ? answer->find("key_packages") : nullptr;
        const auto* items = list == nullptr ? nullptr : list->as_array();
        if (items == nullptr) {
            return out;
        }
        for (const core::json::Value& item : *items) {
            const auto bytes = infra::auth::decode_base64url_bytes(
                item.find("key_package")->as_string().value_or(""));
            const core::json::Value* last = item.find("last_resort");
            out.emplace_back(std::string(item.find("device")->as_string().value_or("")),
                             std::pair{bytes.value_or(KeyPackageBytes{}),
                                       last != nullptr && last->as_bool().value_or(false)});
        }
        return out;
    }

    static std::vector<std::string> devices_in(const std::optional<core::json::Value>& answer,
                                               std::string_view key) {
        std::vector<std::string> out;
        const core::json::Value* list = answer ? answer->find(key) : nullptr;
        const auto* items = list == nullptr ? nullptr : list->as_array();
        if (items != nullptr) {
            for (const core::json::Value& item : *items) {
                out.emplace_back(item.as_string().value_or(""));
            }
        }
        return out;
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::e2ee::MemoryDirectory> store_;
    std::unique_ptr<FakeAccess> access_;
    std::unique_ptr<chat::KeyDirectory> directory_;
};

TEST_F(KeyDirectoryTest, ADevicePublishesAndAPeerClaimsEachPackageOnceThenTheLastResort) {
    FakeClient alice_socket;
    FakeClient bob_socket;
    const auto alice = attach(alice_socket, "alice");
    const auto bob = attach(bob_socket, "bob");
    const std::string laptop = new_device();
    ASSERT_NO_FATAL_FAILURE(
        enrol(alice_socket, alice, laptop, {package(1), package(2)}, package(9)));

    const std::string claim = R"({"type":"claim_key_packages","user":"alice","id":"c1"})";
    std::set<KeyPackageBytes> seen;
    for (int i = 0; i < 2; ++i) {
        const auto answer = ask(bob_socket, bob, claim);
        ASSERT_EQ(type_of(answer), "key_packages") << bob_socket.frames.back();
        EXPECT_EQ(field(answer, "user"), "alice");
        EXPECT_EQ(field(answer, "id"), "c1");
        const auto got = claimed(answer);
        ASSERT_EQ(got.size(), 1U);
        EXPECT_EQ(got[0].first, laptop);
        EXPECT_FALSE(got[0].second.second);
        EXPECT_TRUE(seen.insert(got[0].second.first).second) << "handed out twice";
    }
    EXPECT_EQ(seen, (std::set<KeyPackageBytes>{package(1), package(2)}));

    const auto last = ask(bob_socket, bob, claim);
    const auto got = claimed(last);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].second.first, package(9));
    EXPECT_TRUE(got[0].second.second) << "the last resort is marked as such";

    // The device's own connection heard that it should publish more.
    EXPECT_TRUE(std::ranges::any_of(alice_socket.frames, [&](const std::string& f) {
        return f == R"({"type":"replenish","device":")" + laptop + R"("})";
    }));
    const auto own = ask(alice_socket, alice, R"({"type":"devices","user":"alice"})");
    ASSERT_EQ(type_of(own), "devices");
    EXPECT_NE(own->find("devices")->as_array()->front().find("key_packages"), nullptr);
    EXPECT_EQ(std::string(own->find("devices")
                              ->as_array()
                              ->front()
                              .find("last_resort")
                              ->as_string()
                              .value_or("")),
              "used");

    const auto& c = directory_->counters();
    EXPECT_EQ(c.registered, 1U);
    EXPECT_EQ(c.published, 2U);
    EXPECT_EQ(c.last_resorts_published, 1U);
    EXPECT_EQ(c.claims, 3U);
    EXPECT_EQ(c.claimed, 3U);
    EXPECT_EQ(c.claimed_last_resort, 1U);
    EXPECT_GE(c.replenish_sent, 1U);
}

TEST_F(KeyDirectoryTest, EveryLiveDeviceOfAUserIsClaimedAndAnExhaustedOneIsNamed) {
    FakeClient alice_socket;
    FakeClient bob_socket;
    const auto alice = attach(alice_socket, "alice");
    const auto bob = attach(bob_socket, "bob");
    const std::string phone = new_device();
    const std::string laptop = new_device();
    const std::string tablet = new_device();
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, phone, {package(1)}));
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, laptop, {package(2)}));
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, tablet, {package(3)}));
    const auto retired =
        ask(alice_socket, alice, R"({"type":"retire_device","device":")" + tablet + R"("})");
    ASSERT_EQ(type_of(retired), "device_retired");

    const auto listed = ask(bob_socket, bob, R"({"type":"devices","user":"alice"})");
    ASSERT_EQ(type_of(listed), "devices");
    const auto* entries = listed->find("devices")->as_array();
    ASSERT_EQ(entries->size(), 2U);
    for (const core::json::Value& entry : *entries) {
        EXPECT_EQ(entry.find("key_packages"), nullptr) << "another user's supply is not shown";
    }

    const auto all = ask(bob_socket, bob, R"({"type":"claim_key_packages","user":"alice"})");
    const auto got = claimed(all);
    ASSERT_EQ(got.size(), 2U);
    const std::set<std::string> devices{got[0].first, got[1].first};
    EXPECT_EQ(devices, (std::set<std::string>{phone, laptop}));

    // Named devices: one with nothing left, one retired, one nobody has.
    const std::string stranger = new_device();
    const auto named = ask(bob_socket, bob,
                           R"({"type":"claim_key_packages","user":"alice","devices":[")" + phone +
                               R"(",")" + tablet + R"(",")" + stranger + R"("]})");
    EXPECT_TRUE(claimed(named).empty());
    EXPECT_EQ(devices_in(named, "exhausted"), std::vector<std::string>{phone});
    auto gone = devices_in(named, "gone");
    std::ranges::sort(gone);
    std::vector<std::string> expected{tablet, stranger};
    std::ranges::sort(expected);
    EXPECT_EQ(gone, expected);
    EXPECT_TRUE(devices_in(named, "unavailable").empty());
}

TEST_F(KeyDirectoryTest, OnlySomeoneWhoSharesAChatMayListOrClaimAnotherUsersDevices) {
    FakeClient alice_socket;
    FakeClient mallory_socket;
    const auto alice = attach(alice_socket, "alice");
    const auto mallory = attach(mallory_socket, "mallory");
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, new_device(), {package(1)}));

    for (const std::string text : {R"({"type":"devices","user":"alice","id":"x"})",
                                   R"({"type":"claim_key_packages","user":"alice","id":"x"})"}) {
        const auto refused = ask(mallory_socket, mallory, text);
        EXPECT_EQ(type_of(refused), "error");
        EXPECT_EQ(field(refused, "reason"), "not_shared");
        EXPECT_EQ(field(refused, "user"), "alice");
        EXPECT_EQ(field(refused, "id"), "x");
    }
    // Nothing was taken: alice's own claim still finds her package.
    const int asked = access_->asked;
    const auto own = ask(alice_socket, alice, R"({"type":"claim_key_packages","user":"alice"})");
    EXPECT_EQ(claimed(own).size(), 1U);
    EXPECT_EQ(access_->asked, asked) << "one's own devices need no shared chat";
    EXPECT_EQ(directory_->counters().not_shared, 2U);

    access_->failing = true;
    const auto unsure = ask(mallory_socket, mallory, R"({"type":"devices","user":"alice"})");
    EXPECT_EQ(field(unsure, "reason"), "unavailable");
}

TEST_F(KeyDirectoryTest, ADeviceIsPublishedToOnlyByItsOwnUser) {
    FakeClient alice_socket;
    FakeClient bob_socket;
    const auto alice = attach(alice_socket, "alice");
    const auto bob = attach(bob_socket, "bob");
    const std::string laptop = new_device();
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, laptop, {package(1)}));

    const auto publish = ask(bob_socket, bob,
                             R"({"type":"publish_key_packages","device":")" + laptop +
                                 R"(","key_packages":[")" + encoded(package(7)) + R"("]})");
    EXPECT_EQ(field(publish, "reason"), "unknown_device");
    EXPECT_EQ(field(publish, "device"), laptop);
    const auto take_over =
        ask(bob_socket, bob, R"({"type":"register_device","device":")" + laptop + R"("})");
    EXPECT_EQ(field(take_over, "reason"), "unknown_device");
    const auto retire =
        ask(bob_socket, bob, R"({"type":"retire_device","device":")" + laptop + R"("})");
    EXPECT_EQ(field(retire, "reason"), "unknown_device");

    // Alice's package is the one handed out.
    const auto answer = ask(bob_socket, bob, R"({"type":"claim_key_packages","user":"alice"})");
    const auto got = claimed(answer);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].second.first, package(1));
}

TEST_F(KeyDirectoryTest, ARetiredDeviceStaysRetired) {
    FakeClient alice_socket;
    const auto alice = attach(alice_socket, "alice");
    const std::string laptop = new_device();
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, laptop, {package(1)}));
    ASSERT_EQ(type_of(ask(alice_socket, alice,
                          R"({"type":"retire_device","device":")" + laptop + R"("})")),
              "device_retired");
    EXPECT_EQ(field(ask(alice_socket, alice,
                        R"({"type":"register_device","device":")" + laptop + R"("})"),
                    "reason"),
              "device_retired");
    EXPECT_EQ(field(ask(alice_socket, alice,
                        R"({"type":"publish_key_packages","device":")" + laptop +
                            R"(","last_resort":")" + encoded(package(2)) + R"("})"),
                    "reason"),
              "device_retired");
}

TEST_F(KeyDirectoryTest, AUserHasAtMostSixteenLiveDevicesAndAHundredPackagesEach) {
    make(chat::DirectoryLimits{.read_burst = 120,
                               .reads_per_second = 2,
                               .write_burst = 100,
                               .write_interval = core::Millis{3'000},
                               .max_in_flight = 8});
    FakeClient alice_socket;
    const auto alice = attach(alice_socket, "alice");
    std::string first;
    for (std::size_t i = 0; i < core::ports::kMaxDevicesPerUser; ++i) {
        const std::string device = new_device();
        if (first.empty()) {
            first = device;
        }
        ASSERT_EQ(type_of(ask(alice_socket, alice,
                              R"({"type":"register_device","device":")" + device + R"("})")),
                  "device_registered");
    }
    const auto over = ask(alice_socket, alice,
                          R"({"type":"register_device","device":")" + new_device() + R"("})");
    EXPECT_EQ(field(over, "reason"), "device_limit");
    EXPECT_EQ(directory_->counters().device_limit, 1U);

    std::string list;
    for (std::size_t i = 0; i < 60; ++i) {
        list +=
            (list.empty() ? "\"" : ",\"") + encoded(package(static_cast<std::uint8_t>(i))) + "\"";
    }
    const std::string publish = R"({"type":"publish_key_packages","device":")" + first +
                                R"(","key_packages":[)" + list + "]}";
    ASSERT_EQ(number(ask(alice_socket, alice, publish), "key_packages"), 60U);
    const auto full = ask(alice_socket, alice, publish);
    EXPECT_EQ(field(full, "reason"), "key_packages_full");
}

TEST_F(KeyDirectoryTest, WritesAndReadsAreRateLimitedPerUser) {
    make(chat::DirectoryLimits{.read_burst = 2,
                               .reads_per_second = 1,
                               .write_burst = 2,
                               .write_interval = core::Millis{3'000},
                               .max_in_flight = 8});
    FakeClient one;
    FakeClient two;
    const auto first = attach(one, "alice");
    const auto second = attach(two, "alice");
    const std::string device = new_device();
    const std::string reg = R"({"type":"register_device","device":")" + device + R"("})";
    EXPECT_EQ(type_of(ask(one, first, reg)), "device_registered");
    EXPECT_EQ(type_of(ask(two, second, reg)), "device_registered");
    const auto limited = ask(one, first, reg);
    EXPECT_EQ(field(limited, "reason"), "rate_limited");
    EXPECT_EQ(number(limited, "retry_after_ms"), 3'000U) << "the user's, across connections";
    clock_.advance(core::Millis{3'000});
    EXPECT_EQ(type_of(ask(one, first, reg)), "device_registered");

    const std::string list = R"({"type":"devices","user":"alice"})";
    EXPECT_EQ(type_of(ask(one, first, list)), "devices");
    EXPECT_EQ(type_of(ask(two, second, list)), "devices");
    const auto read_limited = ask(one, first, list);
    EXPECT_EQ(field(read_limited, "reason"), "rate_limited");
    EXPECT_EQ(number(read_limited, "retry_after_ms"), 1'000U);
    EXPECT_EQ(directory_->counters().rate_limited, 2U);
}

TEST_F(KeyDirectoryTest, AConnectionHasAFewCommandsInFlightAtMost) {
    make(chat::DirectoryLimits{.read_burst = 120,
                               .reads_per_second = 2,
                               .write_burst = 20,
                               .write_interval = core::Millis{3'000},
                               .max_in_flight = 2});
    FakeClient client;
    const auto id = attach(client, "alice");
    for (int i = 0; i < 3; ++i) {
        send(id, R"({"type":"devices","user":"alice","id":"q)" + std::to_string(i) + R"("})");
    }
    ASSERT_EQ(client.frames.size(), 1U) << "the third is refused at once";
    EXPECT_EQ(client.frames[0], R"({"type":"error","reason":"busy","id":"q2","user":"alice"})");
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return client.frames.size() == 3; }));
    EXPECT_EQ(directory_->counters().busy, 1U);
    // Answered: places are free again.
    EXPECT_EQ(type_of(ask(client, id, R"({"type":"devices","user":"alice"})")), "devices");
}

TEST_F(KeyDirectoryTest, WithoutADirectoryEveryCommandIsUnavailable) {
    make(chat::DirectoryLimits{}, false);
    FakeClient client;
    const auto id = attach(client, "alice");
    send(id, R"({"type":"register_device","device":")" + new_device() + R"(","id":"r"})");
    ASSERT_EQ(client.frames.size(), 1U);
    EXPECT_NE(client.frames[0].find(R"("reason":"unavailable")"), std::string::npos);
}

TEST_F(KeyDirectoryTest, AnswersToAClientThatLeftOrAServiceThatWentAreDropped) {
    FakeClient alice_socket;
    const auto alice = attach(alice_socket, "alice");
    ASSERT_NO_FATAL_FAILURE(enrol(alice_socket, alice, new_device(), {package(1)}));
    const std::size_t before = alice_socket.frames.size();
    send(alice, R"({"type":"devices","user":"alice"})");
    directory_->detach(alice);
    ulw::test::pump_pending(*reactor_);
    EXPECT_EQ(alice_socket.frames.size(), before);

    FakeClient again;
    const auto id = attach(again, "alice");
    send(id, R"({"type":"claim_key_packages","user":"alice"})");
    directory_.reset();
    ulw::test::pump_pending(*reactor_);
    EXPECT_TRUE(again.frames.empty());
}

} // namespace
