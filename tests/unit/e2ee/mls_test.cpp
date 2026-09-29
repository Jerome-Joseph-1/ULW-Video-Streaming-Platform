#include "infra/e2ee/mls.hpp"

#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using infra::e2ee::MlsBytes;
using infra::e2ee::MlsClient;
using infra::e2ee::MlsError;
using infra::e2ee::MlsGroup;
using infra::e2ee::MlsReceived;

MlsBytes bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span{text});
    return {view.begin(), view.end()};
}

MlsClient client(std::string_view identity) {
    auto made = MlsClient::create(bytes(identity));
    EXPECT_TRUE(made) << to_string(made.error());
    return std::move(*made);
}

// Alice starts the group and adds Bob and Carol by their key packages; everyone is at epoch 1.
struct Trio {
    MlsClient alice_device = client("alice-phone");
    MlsClient bob_device = client("bob-laptop");
    MlsClient carol_device = client("carol-tablet");
    std::optional<MlsGroup> alice;
    std::optional<MlsGroup> bob;
    std::optional<MlsGroup> carol;

    void form() {
        auto created = alice_device.create_group(bytes("room-7"));
        ASSERT_TRUE(created);
        alice = std::move(*created);
        const auto bob_package = bob_device.key_package();
        const auto carol_package = carol_device.key_package();
        ASSERT_TRUE(bob_package && carol_package);
        const std::vector<MlsBytes> packages{*bob_package, *carol_package};
        const auto added = alice->add(packages);
        ASSERT_TRUE(added) << to_string(added.error());
        ASSERT_TRUE(alice->merge_pending_commit());
        auto bob_joined = bob_device.join(added->welcome);
        ASSERT_TRUE(bob_joined) << to_string(bob_joined.error());
        bob = std::move(*bob_joined);
        auto carol_joined = carol_device.join(added->welcome);
        ASSERT_TRUE(carol_joined) << to_string(carol_joined.error());
        carol = std::move(*carol_joined);
    }

    // Alice removes Carol and the delivery service accepts it; only Bob hears of it.
    void remove_carol() {
        const auto removal = alice->remove(bytes("carol-tablet"));
        ASSERT_TRUE(removal) << to_string(removal.error());
        ASSERT_TRUE(alice->merge_pending_commit());
        const auto seen = bob->process(*removal);
        ASSERT_TRUE(seen);
        ASSERT_EQ(seen->kind, MlsReceived::Commit);
        removal_commit = *removal;
    }

    MlsBytes removal_commit;
};

void expect_reads(MlsGroup& reader, const MlsBytes& ciphertext, std::string_view plaintext) {
    const auto got = reader.process(ciphertext);
    ASSERT_TRUE(got) << to_string(got.error());
    EXPECT_EQ(got->kind, MlsReceived::Application);
    EXPECT_EQ(got->plaintext, bytes(plaintext));
}

TEST(Mls, ThreeDevicesFormAGroupAndReadEachOther) {
    Trio t;
    t.form();
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    for (MlsGroup* g : {&*t.alice, &*t.bob, &*t.carol}) {
        EXPECT_EQ(g->member_count(), 3U);
        EXPECT_EQ(g->epoch(), 1U);
    }

    const auto from_alice = t.alice->encrypt(bytes("from alice"));
    const auto from_bob = t.bob->encrypt(bytes("from bob"));
    const auto from_carol = t.carol->encrypt(bytes("from carol"));
    ASSERT_TRUE(from_alice && from_bob && from_carol);
    EXPECT_NE(*from_alice, bytes("from alice"));
    expect_reads(*t.bob, *from_alice, "from alice");
    expect_reads(*t.carol, *from_alice, "from alice");
    expect_reads(*t.alice, *from_bob, "from bob");
    expect_reads(*t.carol, *from_bob, "from bob");
    expect_reads(*t.alice, *from_carol, "from carol");
    expect_reads(*t.bob, *from_carol, "from carol");
}

TEST(Mls, RemovedDeviceCannotReadTheNextEpoch) {
    Trio t;
    t.form();
    t.remove_carol();
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const auto processed = t.carol->process(t.removal_commit);
    ASSERT_TRUE(processed) << to_string(processed.error());
    EXPECT_EQ(processed->kind, MlsReceived::Commit);
    EXPECT_EQ(t.alice->member_count(), 2U);
    EXPECT_EQ(t.bob->epoch(), 2U);

    const auto after = t.alice->encrypt(bytes("carol is gone"));
    ASSERT_TRUE(after);
    // The same ciphertext Bob reads, so Carol's failure is hers and not a broken message.
    expect_reads(*t.bob, *after, "carol is gone");
    const auto carol_reads = t.carol->process(*after);
    ASSERT_FALSE(carol_reads) << "a removed device decrypted the next epoch";
    EXPECT_EQ(carol_reads.error(), MlsError::Inactive);
    EXPECT_EQ(t.carol->encrypt(bytes("still here?")).error(), MlsError::Inactive);
}

TEST(Mls, RemovedDeviceThatMissedTheCommitCannotReadTheNextEpochEither) {
    Trio t;
    t.form();
    t.remove_carol();
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const auto after = t.alice->encrypt(bytes("carol is gone"));
    ASSERT_TRUE(after);
    expect_reads(*t.bob, *after, "carol is gone");
    // Carol is still at epoch 1 and holds none of epoch 2's secrets.
    const auto carol_reads = t.carol->process(*after);
    ASSERT_FALSE(carol_reads) << "a removed device decrypted the next epoch";
    EXPECT_EQ(carol_reads.error(), MlsError::Rejected);
}

TEST(Mls, WelcomeCannotBeUsedTwice) {
    const MlsClient alice_device = client("alice-phone");
    const MlsClient bob_device = client("bob-laptop");
    auto alice = alice_device.create_group(bytes("room-7"));
    const auto package = bob_device.key_package();
    ASSERT_TRUE(alice && package);
    const std::vector<MlsBytes> packages{*package};
    const auto added = alice->add(packages);
    ASSERT_TRUE(added);
    ASSERT_TRUE(bob_device.join(added->welcome));
    // Joining spent the key package's private init key.
    EXPECT_EQ(bob_device.join(added->welcome).error(), MlsError::Rejected);
}

TEST(Mls, LoserOfAnEpochDropsItsCommitAndFollowsTheWinner) {
    Trio t;
    t.form();
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const auto alices = t.alice->remove(bytes("carol-tablet"));
    const auto bobs = t.bob->remove(bytes("alice-phone"));
    ASSERT_TRUE(alices && bobs);
    // The delivery service took Alice's commit for epoch 1 and refused Bob's.
    ASSERT_TRUE(t.alice->merge_pending_commit());
    ASSERT_TRUE(t.bob->clear_pending_commit());
    const auto seen = t.bob->process(*alices);
    ASSERT_TRUE(seen) << to_string(seen.error());
    EXPECT_EQ(t.bob->epoch(), 2U);
    EXPECT_EQ(t.bob->member_count(), 2U);
    const auto hello = t.bob->encrypt(bytes("agreed"));
    ASSERT_TRUE(hello);
    expect_reads(*t.alice, *hello, "agreed");
}

std::size_t threads_in_this_process() {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{"/proc/self/task"},
                      std::filesystem::directory_iterator{}));
}

// OpenMLS computes update paths with rayon, which on its own starts a pool of threads; the
// bridge keeps that work on the caller's thread (ADR-0044). ctest runs every test in a process
// of its own, so a pool started here cannot hide behind one another test started first.
TEST(Mls, NeverStartsAThread) {
    const std::size_t before = threads_in_this_process();
    Trio t;
    t.form();
    t.remove_carol();
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const auto hello = t.alice->encrypt(bytes("one thread"));
    ASSERT_TRUE(hello);
    expect_reads(*t.bob, *hello, "one thread");
    EXPECT_EQ(threads_in_this_process(), before);
}

TEST(Mls, RemovingSomeoneOutsideTheGroupIsNotAMember) {
    Trio t;
    t.form();
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    EXPECT_EQ(t.alice->remove(bytes("mallory")).error(), MlsError::NotAMember);
}

TEST(Mls, KeyPackageFitsTheDirectorysSizeBound) {
    const auto package = client("0190a3c2-7d1e-7f00-8000-000000000001").key_package();
    ASSERT_TRUE(package);
    // core::ports::kMaxKeyPackageBytes. With a basic credential a package is about 300 bytes.
    EXPECT_LT(package->size(), 8192U);
}

// M21's acceptance run. Under the asan preset LeakSanitizer sees every allocation on both sides
// of the boundary, since Rust allocates through the same malloc, and ASan checks every access
// the C++ side makes, handing buffers in and freeing what comes back. It cannot see a bad
// access made inside the Rust code, which is not instrumented (ADR-0044): that side rests on
// safe Rust, the unsafe blocks at the boundary, and the fuzzer.
TEST(Mls, SurvivesAThousandCreateAddRemoveEncryptDecryptCycles) {
    constexpr int kCycles = 1000;
    for (int i = 0; i < kCycles; ++i) {
        Trio t;
        t.form();
        const auto hello = t.bob->encrypt(bytes("cycle"));
        ASSERT_TRUE(hello);
        expect_reads(*t.carol, *hello, "cycle");
        t.remove_carol();
        const auto after = t.alice->encrypt(bytes("after"));
        ASSERT_TRUE(after);
        expect_reads(*t.bob, *after, "after");
        ASSERT_FALSE(t.carol->process(*after));
        ASSERT_FALSE(testing::Test::HasFailure()) << "cycle " << i;
    }
}

} // namespace
