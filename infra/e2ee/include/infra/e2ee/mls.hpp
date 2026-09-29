#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The bridge's opaque handles; only their names are needed here.
struct UlwMlsClient;
struct UlwMlsGroup;

namespace infra::e2ee {

enum class MlsError : std::uint8_t {
    // A null or empty argument where one is required: a bug in the caller.
    InvalidArgument,
    // The bytes are not the MLS structure the call expects.
    Malformed,
    // Well-formed, but the protocol refuses it: another group or epoch, a bad signature, a
    // welcome for someone else, a key package that was already used.
    Rejected,
    NotAMember,
    // This member was removed; the group is dead to it.
    Inactive,
    // The bridge failed inside; drop the objects involved.
    Internal,
};

[[nodiscard]] std::string_view to_string(MlsError e) noexcept;

template <class T> using MlsResult = std::expected<T, MlsError>;

using MlsBytes = std::vector<std::byte>;

class MlsGroup;

// One device's MLS identity (ADR-0039). Client-side only: the server never holds one.
class MlsClient {
public:
    // `identity` goes into the device's credential; the device id, 1 to 64 bytes.
    [[nodiscard]] static MlsResult<MlsClient> create(std::span<const std::byte> identity);

    // A single-use KeyPackage to publish to the directory.
    [[nodiscard]] MlsResult<MlsBytes> key_package() const;
    // A group at epoch 0 with this device as its only member.
    [[nodiscard]] MlsResult<MlsGroup> create_group(std::span<const std::byte> group_id) const;
    [[nodiscard]] MlsResult<MlsGroup> join(std::span<const std::byte> welcome) const;

private:
    struct Free {
        void operator()(UlwMlsClient* client) const noexcept;
    };

    explicit MlsClient(UlwMlsClient* client) noexcept : client_(client) {}

    std::unique_ptr<UlwMlsClient, Free> client_;
};

struct MlsAddition {
    // For the members already in the group.
    MlsBytes commit;
    // For the members being added.
    MlsBytes welcome;
};

enum class MlsReceived : std::uint8_t { Application, Commit, Proposal };

struct MlsIncoming {
    MlsReceived kind = MlsReceived::Application;
    // Set for an application message only.
    MlsBytes plaintext;
};

// One device's view of one group. add() and remove() leave their commit pending until
// merge_pending_commit() (the delivery service accepted it for this epoch) or
// clear_pending_commit() (another commit won the epoch).
class MlsGroup {
public:
    [[nodiscard]] MlsResult<MlsAddition> add(std::span<const MlsBytes> key_packages);
    [[nodiscard]] MlsResult<MlsBytes> remove(std::span<const std::byte> identity);
    [[nodiscard]] MlsResult<void> merge_pending_commit();
    [[nodiscard]] MlsResult<void> clear_pending_commit();

    [[nodiscard]] MlsResult<MlsBytes> encrypt(std::span<const std::byte> plaintext);
    // A message from another member: decrypts it, merges it or queues it.
    [[nodiscard]] MlsResult<MlsIncoming> process(std::span<const std::byte> message);

    [[nodiscard]] MlsResult<std::uint64_t> epoch() const;
    [[nodiscard]] MlsResult<std::size_t> member_count() const;

private:
    friend class MlsClient;

    struct Free {
        void operator()(UlwMlsGroup* group) const noexcept;
    };

    explicit MlsGroup(UlwMlsGroup* group) noexcept : group_(group) {}

    std::unique_ptr<UlwMlsGroup, Free> group_;
};

} // namespace infra::e2ee
