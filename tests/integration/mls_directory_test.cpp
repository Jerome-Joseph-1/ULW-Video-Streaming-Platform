// Three devices build a group the way clients will: key packages published to and fetched from
// the Postgres directory, and each commit merged only once the directory has accepted it for
// its epoch. The server side sees nothing but opaque bytes and epoch numbers.

#include "infra/e2ee/mls.hpp"

#include "e2ee_directory_contract.hpp"
#include "pg_directory_harness.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using core::ports::E2eeError;
using core::ports::E2eeResult;
using infra::e2ee::MlsBytes;
using infra::e2ee::MlsClient;
using infra::e2ee::MlsError;
using infra::e2ee::MlsGroup;
using infra::e2ee::MlsReceived;
using ulw::test::Answer;
using ulw::test::PgDirectoryHarness;

MlsBytes bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span{text});
    return {view.begin(), view.end()};
}

// A person's device: its directory entry and its MLS client, named by its device id.
struct Device {
    core::UserId user;
    core::DeviceId id;
    MlsClient mls;
    std::optional<MlsGroup> group;
};

class MlsThroughDirectory : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = PgDirectoryHarness::open(4);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        ASSERT_NE(harness_, nullptr);
    }

    template <class T, class Start> E2eeResult<T> call(Start start) {
        Answer<T> answer;
        start(answer.callback());
        return ulw::test::await(harness_->reactor(), answer);
    }

    // Registers a device and publishes `packages` key packages for it.
    Device enrol(std::string_view user, int packages) {
        const auto id = core::DeviceId::generate(clock_, random_);
        auto mls = MlsClient::create(bytes(id.to_string()));
        EXPECT_TRUE(mls);
        Device d{.user = *core::UserId::parse(user),
                 .id = id,
                 .mls = std::move(*mls),
                 .group = std::nullopt};
        EXPECT_TRUE(call<void>([&](auto done) {
            harness_->registry().register_device(d.user, d.id, std::move(done));
        }));
        std::vector<core::ports::KeyPackageBytes> batch;
        for (int i = 0; i < packages; ++i) {
            auto package = d.mls.key_package();
            EXPECT_TRUE(package);
            batch.push_back(std::move(*package));
        }
        EXPECT_TRUE(call<std::size_t>([&](auto done) {
            harness_->delivery().publish_key_packages(d.user, d.id, std::move(batch),
                                                      std::move(done));
        }));
        return d;
    }

    E2eeResult<core::ports::FetchedKeyPackage> fetch(const Device& d) {
        return call<core::ports::FetchedKeyPackage>([&](auto done) {
            harness_->delivery().fetch_key_package(d.user, d.id, std::move(done));
        });
    }

    E2eeResult<void> claim(const Device& committer, const MlsBytes& commit) {
        const auto epoch = committer.group->epoch();
        EXPECT_TRUE(epoch);
        return call<void>([&](auto done) {
            harness_->delivery().submit_commit(room_, committer.user, committer.id, *epoch, commit,
                                               std::move(done));
        });
    }

    E2eeResult<std::vector<core::ports::StoredCommit>> commits_from(std::uint64_t epoch) {
        return call<std::vector<core::ports::StoredCommit>>(
            [&](auto done) { harness_->delivery().fetch_commits(room_, epoch, std::move(done)); });
    }

    static void expect_reads(Device& reader, const MlsBytes& ciphertext,
                             std::string_view plaintext) {
        const auto got = reader.group->process(ciphertext);
        ASSERT_TRUE(got) << to_string(got.error());
        EXPECT_EQ(got->plaintext, bytes(plaintext));
    }

    ulw::test::FakeClock clock_;
    ulw::test::FakeRandom random_;
    const core::RoomId room_ = core::RoomId::generate(clock_, random_);
    std::unique_ptr<PgDirectoryHarness> harness_;
};

TEST_F(MlsThroughDirectory, ThreeDevicesJoinThroughTheDirectoryAndOrderTheirCommits) {
    Device alice = enrol("auth0|alice", 1);
    Device bob = enrol("auth0|bob", 3);
    Device carol = enrol("auth0|carol", 3);

    const auto room_id = bytes(room_.to_string());
    auto created = alice.mls.create_group(room_id);
    ASSERT_TRUE(created);
    alice.group = std::move(*created);
    const auto bob_package = fetch(bob);
    const auto carol_package = fetch(carol);
    ASSERT_TRUE(bob_package && carol_package);
    const std::vector<MlsBytes> packages{bob_package->package, carol_package->package};
    const auto added = alice.group->add(packages);
    ASSERT_TRUE(added) << to_string(added.error());
    ASSERT_TRUE(claim(alice, added->commit));
    ASSERT_TRUE(alice.group->merge_pending_commit());
    auto bob_joined = bob.mls.join(added->welcome);
    auto carol_joined = carol.mls.join(added->welcome);
    ASSERT_TRUE(bob_joined && carol_joined);
    bob.group = std::move(*bob_joined);
    carol.group = std::move(*carol_joined);

    const auto hello = carol.group->encrypt(bytes("hi both"));
    ASSERT_TRUE(hello);
    expect_reads(alice, *hello, "hi both");
    expect_reads(bob, *hello, "hi both");

    // Alice and Bob both commit at epoch 1; the directory takes whichever claims first.
    const auto alices = alice.group->remove(bytes(carol.id.to_string()));
    const auto bobs = bob.group->remove(bytes(alice.id.to_string()));
    ASSERT_TRUE(alices && bobs);
    ASSERT_TRUE(claim(alice, *alices));
    EXPECT_EQ(claim(bob, *bobs).error(), E2eeError::StaleEpoch);
    ASSERT_TRUE(alice.group->merge_pending_commit());
    ASSERT_TRUE(bob.group->clear_pending_commit());
    const auto bob_follows = bob.group->process(*alices);
    ASSERT_TRUE(bob_follows) << to_string(bob_follows.error());
    EXPECT_EQ(bob_follows->kind, MlsReceived::Commit);
    // Carol missed the room's copy and catches up from the directory, which kept the winner.
    const auto missed = commits_from(1);
    ASSERT_TRUE(missed);
    ASSERT_EQ(missed->size(), 1U);
    EXPECT_EQ(missed->front().commit, *alices);
    ASSERT_TRUE(carol.group->process(missed->front().commit));

    const auto after = bob.group->encrypt(bytes("just us"));
    ASSERT_TRUE(after);
    expect_reads(alice, *after, "just us");
    const auto carol_reads = carol.group->process(*after);
    ASSERT_FALSE(carol_reads) << "a removed device decrypted the next epoch";
    EXPECT_EQ(carol_reads.error(), MlsError::Inactive);

    // Bob's second package is still there for the next invitation; the first is spent. Alice
    // published one, and a second invitation finds her out.
    const auto next = fetch(bob);
    ASSERT_TRUE(next);
    EXPECT_NE(next->package, bob_package->package);
    ASSERT_TRUE(fetch(alice));
    EXPECT_EQ(fetch(alice).error(), E2eeError::Exhausted);
}

} // namespace
