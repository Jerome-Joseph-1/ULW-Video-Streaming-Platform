// The bridge's C API as a hostile or careless caller uses it: null handles and outputs, lengths
// without data, garbage, every truncation of real messages and flipped bits. Each call must
// answer with a status and leave its outputs empty; none may crash, leak or corrupt the group.

#include "ulw/mls_ffi_bridge.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

struct FreeClient {
    void operator()(UlwMlsClient* c) const noexcept { ulw_mls_client_free(c); }
};
struct FreeGroup {
    void operator()(UlwMlsGroup* g) const noexcept { ulw_mls_group_free(g); }
};
using Client = std::unique_ptr<UlwMlsClient, FreeClient>;
using Group = std::unique_ptr<UlwMlsGroup, FreeGroup>;

Bytes bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

// Takes what the bridge wrote into `buffer` and releases it.
Bytes take(UlwMlsBuffer buffer) {
    const std::span<const std::uint8_t> view{buffer.data, buffer.len};
    Bytes out(view.begin(), view.end());
    ulw_mls_buffer_free(buffer);
    return out;
}

Client make_client(std::string_view identity) {
    UlwMlsClient* raw = nullptr;
    const Bytes id = bytes(identity);
    EXPECT_EQ(ulw_mls_client_new(id.data(), id.size(), &raw), ULW_MLS_STATUS_OK);
    return Client{raw};
}

Bytes key_package(const Client& c) {
    UlwMlsBuffer out{};
    EXPECT_EQ(ulw_mls_client_key_package(c.get(), &out), ULW_MLS_STATUS_OK);
    return take(out);
}

// Alice's group with Bob in it, and one message of every kind the bridge decodes.
class MlsFfi : public ::testing::Test {
protected:
    void SetUp() override {
        const Bytes group_id = bytes("room-9");
        UlwMlsGroup* raw = nullptr;
        ASSERT_EQ(ulw_mls_group_create(alice_device.get(), group_id.data(), group_id.size(), &raw),
                  ULW_MLS_STATUS_OK);
        alice.reset(raw);
        bob_package = key_package(bob_device);
        const UlwMlsBytes package{.data = bob_package.data(), .len = bob_package.size()};
        UlwMlsBuffer commit_out{};
        UlwMlsBuffer welcome_out{};
        ASSERT_EQ(ulw_mls_group_add(alice.get(), &package, 1, &commit_out, &welcome_out),
                  ULW_MLS_STATUS_OK);
        add_commit = take(commit_out);
        welcome = take(welcome_out);
        ASSERT_EQ(ulw_mls_group_merge_pending_commit(alice.get()), ULW_MLS_STATUS_OK);
        ASSERT_EQ(ulw_mls_group_join(bob_device.get(), welcome.data(), welcome.size(), &raw),
                  ULW_MLS_STATUS_OK);
        bob.reset(raw);
        application = encrypt(alice, "hello bob");
        spare_package = key_package(carol_device);
    }

    static Bytes encrypt(const Group& g, std::string_view text) {
        const Bytes plain = bytes(text);
        UlwMlsBuffer out{};
        EXPECT_EQ(ulw_mls_group_encrypt(g.get(), plain.data(), plain.size(), &out),
                  ULW_MLS_STATUS_OK);
        return take(out);
    }

    // Bob processes `message`; the plaintext buffer must be left empty on any failure.
    static UlwMlsStatus process(const Group& g, const Bytes& message, Bytes* plaintext = nullptr) {
        UlwMlsReceived kind = ULW_MLS_RECEIVED_PROPOSAL;
        UlwMlsBuffer out{.data = nullptr, .len = 0};
        const UlwMlsStatus s =
            ulw_mls_group_process(g.get(), message.data(), message.size(), &kind, &out);
        if (s != ULW_MLS_STATUS_OK) {
            EXPECT_EQ(out.data, nullptr);
            EXPECT_EQ(out.len, 0U);
        }
        Bytes got = take(out);
        if (plaintext != nullptr) {
            *plaintext = std::move(got);
        }
        return s;
    }

    [[nodiscard]] UlwMlsStatus join(const Bytes& message) const {
        UlwMlsGroup* raw = nullptr;
        const UlwMlsStatus s =
            ulw_mls_group_join(carol_device.get(), message.data(), message.size(), &raw);
        const Group joined{raw};
        if (s != ULW_MLS_STATUS_OK) {
            EXPECT_EQ(raw, nullptr);
        }
        return s;
    }

    // Leaves Alice's group unchanged whatever happens.
    UlwMlsStatus add(const Bytes& package) {
        const UlwMlsBytes entry{.data = package.data(), .len = package.size()};
        UlwMlsBuffer commit_out{};
        UlwMlsBuffer welcome_out{};
        const UlwMlsStatus s = ulw_mls_group_add(alice.get(), &entry, 1, &commit_out, &welcome_out);
        if (s == ULW_MLS_STATUS_OK) {
            EXPECT_EQ(ulw_mls_group_clear_pending_commit(alice.get()), ULW_MLS_STATUS_OK);
        } else {
            EXPECT_EQ(commit_out.data, nullptr);
            EXPECT_EQ(welcome_out.data, nullptr);
        }
        ulw_mls_buffer_free(commit_out);
        ulw_mls_buffer_free(welcome_out);
        return s;
    }

    // The group still works: Bob reads a fresh message from Alice.
    void expect_intact() {
        Bytes plain;
        ASSERT_EQ(process(bob, encrypt(alice, "still fine"), &plain), ULW_MLS_STATUS_OK);
        EXPECT_EQ(plain, bytes("still fine"));
    }

    Client alice_device = make_client("alice-phone");
    Client bob_device = make_client("bob-laptop");
    Client carol_device = make_client("carol-tablet");
    Group alice;
    Group bob;
    Bytes bob_package;
    Bytes spare_package;
    Bytes add_commit;
    Bytes welcome;
    Bytes application;
};

TEST_F(MlsFfi, NullHandlesAndOutputsAreInvalidArguments) {
    const Bytes id = bytes("x");
    UlwMlsClient* client = nullptr;
    UlwMlsGroup* group = nullptr;
    UlwMlsBuffer buffer{};
    UlwMlsReceived kind{};
    std::uint64_t epoch = 0;
    std::size_t count = 0;
    const UlwMlsBytes entry{.data = spare_package.data(), .len = spare_package.size()};
    constexpr auto kInvalid = ULW_MLS_STATUS_INVALID_ARGUMENT;

    EXPECT_EQ(ulw_mls_client_new(id.data(), id.size(), nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_client_key_package(nullptr, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_client_key_package(alice_device.get(), nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_create(nullptr, id.data(), id.size(), &group), kInvalid);
    EXPECT_EQ(ulw_mls_group_create(alice_device.get(), id.data(), id.size(), nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_join(nullptr, welcome.data(), welcome.size(), &group), kInvalid);
    EXPECT_EQ(ulw_mls_group_join(carol_device.get(), welcome.data(), welcome.size(), nullptr),
              kInvalid);
    EXPECT_EQ(ulw_mls_group_add(nullptr, &entry, 1, &buffer, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_add(alice.get(), nullptr, 1, &buffer, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_add(alice.get(), &entry, 0, &buffer, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_add(alice.get(), &entry, 1, nullptr, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_add(alice.get(), &entry, 1, &buffer, nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_remove(nullptr, id.data(), id.size(), &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_remove(alice.get(), id.data(), id.size(), nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_merge_pending_commit(nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_clear_pending_commit(nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_encrypt(nullptr, id.data(), id.size(), &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_encrypt(alice.get(), id.data(), id.size(), nullptr), kInvalid);
    EXPECT_EQ(
        ulw_mls_group_process(nullptr, application.data(), application.size(), &kind, &buffer),
        kInvalid);
    EXPECT_EQ(
        ulw_mls_group_process(bob.get(), application.data(), application.size(), nullptr, &buffer),
        kInvalid);
    EXPECT_EQ(
        ulw_mls_group_process(bob.get(), application.data(), application.size(), &kind, nullptr),
        kInvalid);
    EXPECT_EQ(ulw_mls_group_epoch(nullptr, &epoch), kInvalid);
    EXPECT_EQ(ulw_mls_group_epoch(alice.get(), nullptr), kInvalid);
    EXPECT_EQ(ulw_mls_group_member_count(nullptr, &count), kInvalid);
    EXPECT_EQ(ulw_mls_group_member_count(alice.get(), nullptr), kInvalid);
    EXPECT_EQ(client, nullptr);
    EXPECT_EQ(group, nullptr);
    // Freeing nothing is allowed.
    ulw_mls_client_free(nullptr);
    ulw_mls_group_free(nullptr);
    ulw_mls_buffer_free(UlwMlsBuffer{.data = nullptr, .len = 0});
    expect_intact();
}

TEST_F(MlsFfi, LengthWithoutDataIsInvalidArgument) {
    UlwMlsClient* client = nullptr;
    UlwMlsGroup* group = nullptr;
    UlwMlsBuffer buffer{};
    UlwMlsReceived kind{};
    const UlwMlsBytes entry{.data = nullptr, .len = 10};
    constexpr auto kInvalid = ULW_MLS_STATUS_INVALID_ARGUMENT;
    EXPECT_EQ(ulw_mls_client_new(nullptr, 10, &client), kInvalid);
    EXPECT_EQ(ulw_mls_group_create(alice_device.get(), nullptr, 10, &group), kInvalid);
    EXPECT_EQ(ulw_mls_group_join(carol_device.get(), nullptr, 10, &group), kInvalid);
    EXPECT_EQ(ulw_mls_group_add(alice.get(), &entry, 1, &buffer, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_remove(alice.get(), nullptr, 10, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_encrypt(alice.get(), nullptr, 10, &buffer), kInvalid);
    EXPECT_EQ(ulw_mls_group_process(bob.get(), nullptr, 10, &kind, &buffer), kInvalid);
    expect_intact();
}

TEST_F(MlsFfi, IdentitiesAndGroupIdsOutOfBoundsAreInvalidArguments) {
    UlwMlsClient* client = nullptr;
    UlwMlsGroup* group = nullptr;
    const Bytes too_long(65, 'a');
    EXPECT_EQ(ulw_mls_client_new(nullptr, 0, &client), ULW_MLS_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(ulw_mls_client_new(too_long.data(), too_long.size(), &client),
              ULW_MLS_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(ulw_mls_group_create(alice_device.get(), nullptr, 0, &group),
              ULW_MLS_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(ulw_mls_group_create(alice_device.get(), too_long.data(), too_long.size(), &group),
              ULW_MLS_STATUS_INVALID_ARGUMENT);
    const Bytes same = bytes("room-9");
    EXPECT_EQ(ulw_mls_group_create(alice_device.get(), same.data(), same.size(), &group),
              ULW_MLS_STATUS_REJECTED)
        << "a client already in a group must not start it again";
}

TEST_F(MlsFfi, GarbageIsMalformedWhereverBytesAreDecoded) {
    for (const Bytes& junk :
         {Bytes{}, Bytes{0x00}, bytes("not an mls message at all"), Bytes(4096, 0xff)}) {
        EXPECT_EQ(join(junk), ULW_MLS_STATUS_MALFORMED);
        EXPECT_EQ(process(bob, junk), ULW_MLS_STATUS_MALFORMED);
        EXPECT_EQ(add(junk), ULW_MLS_STATUS_MALFORMED);
    }
    expect_intact();
}

TEST_F(MlsFfi, MessagesOfTheWrongKindAreMalformed) {
    EXPECT_EQ(join(application), ULW_MLS_STATUS_MALFORMED);
    EXPECT_EQ(join(spare_package), ULW_MLS_STATUS_MALFORMED);
    EXPECT_EQ(process(bob, welcome), ULW_MLS_STATUS_MALFORMED);
    EXPECT_EQ(process(bob, spare_package), ULW_MLS_STATUS_MALFORMED);
    EXPECT_EQ(add(application), ULW_MLS_STATUS_MALFORMED);
    EXPECT_EQ(add(welcome), ULW_MLS_STATUS_MALFORMED);
    expect_intact();
}

TEST_F(MlsFfi, EveryTruncationOfARealMessageIsRefused) {
    const auto prefixes = [](const Bytes& whole) {
        std::vector<Bytes> out;
        out.reserve(whole.size());
        for (std::size_t n = 0; n < whole.size(); ++n) {
            out.emplace_back(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
        }
        return out;
    };
    for (const Bytes& cut : prefixes(welcome)) {
        EXPECT_NE(join(cut), ULW_MLS_STATUS_OK) << cut.size();
    }
    for (const Bytes& cut : prefixes(application)) {
        EXPECT_NE(process(bob, cut), ULW_MLS_STATUS_OK) << cut.size();
    }
    for (const Bytes& cut : prefixes(spare_package)) {
        EXPECT_NE(add(cut), ULW_MLS_STATUS_OK) << cut.size();
    }
    // Carol's package survived all of that and still adds her.
    EXPECT_EQ(add(spare_package), ULW_MLS_STATUS_OK);
    expect_intact();
}

// A tampered copy that decrypts its sender data spends that generation's key, so the genuine
// message behind it is lost as well (RFC 9420 section 9.2 deletes a key once used); the member
// keeps reading everything after it.
TEST_F(MlsFfi, FlippedBitAnywhereInACiphertextIsRejected) {
    for (std::size_t i = 0; i < application.size(); ++i) {
        Bytes tampered = application;
        tampered[i] ^= 0x01U;
        EXPECT_NE(process(bob, tampered), ULW_MLS_STATUS_OK) << "byte " << i;
    }
    expect_intact();
}

TEST_F(MlsFfi, TamperedKeyPackageIsRejected) {
    Bytes tampered = spare_package;
    tampered.back() ^= 0x01U;
    EXPECT_EQ(add(tampered), ULW_MLS_STATUS_REJECTED);
}

TEST_F(MlsFfi, MessageForAnotherGroupIsRejected) {
    const Bytes other_id = bytes("room-10");
    UlwMlsGroup* raw = nullptr;
    ASSERT_EQ(ulw_mls_group_create(carol_device.get(), other_id.data(), other_id.size(), &raw),
              ULW_MLS_STATUS_OK);
    const Group other{raw};
    EXPECT_EQ(process(bob, encrypt(other, "wrong room")), ULW_MLS_STATUS_REJECTED);
    expect_intact();
}

TEST_F(MlsFfi, MergingWithNothingPendingIsRejected) {
    EXPECT_EQ(ulw_mls_group_merge_pending_commit(alice.get()), ULW_MLS_STATUS_REJECTED);
    expect_intact();
}

} // namespace
