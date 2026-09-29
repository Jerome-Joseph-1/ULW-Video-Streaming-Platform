#include "infra/e2ee/mls.hpp"

#include "ulw/mls_ffi_bridge.h"

#include <utility>

namespace infra::e2ee {

namespace {

MlsError to_error(UlwMlsStatus status) noexcept {
    switch (status) {
    // Callers pass only failures; Ok cannot reach here.
    case ULW_MLS_STATUS_OK:
    case ULW_MLS_STATUS_INTERNAL:
        return MlsError::Internal;
    case ULW_MLS_STATUS_INVALID_ARGUMENT:
        return MlsError::InvalidArgument;
    case ULW_MLS_STATUS_MALFORMED:
        return MlsError::Malformed;
    case ULW_MLS_STATUS_REJECTED:
        return MlsError::Rejected;
    case ULW_MLS_STATUS_NOT_A_MEMBER:
        return MlsError::NotAMember;
    case ULW_MLS_STATUS_INACTIVE:
        return MlsError::Inactive;
    }
    return MlsError::Internal;
}

// Owns a buffer the bridge filled, for the length of one call.
class Buffer {
public:
    Buffer() noexcept = default;
    ~Buffer() { ulw_mls_buffer_free(raw_); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) = delete;
    Buffer& operator=(Buffer&&) = delete;

    UlwMlsBuffer* out() noexcept { return &raw_; }

    [[nodiscard]] MlsBytes take() const {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): uint8_t and std::byte.
        const auto* first = reinterpret_cast<const std::byte*>(raw_.data);
        return raw_.data == nullptr ? MlsBytes{} : MlsBytes(first, first + raw_.len);
    }

private:
    UlwMlsBuffer raw_{.data = nullptr, .len = 0};
};

const std::uint8_t* data_of(std::span<const std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): std::byte and uint8_t.
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

} // namespace

std::string_view to_string(MlsError e) noexcept {
    switch (e) {
    case MlsError::InvalidArgument:
        return "invalid argument";
    case MlsError::Malformed:
        return "malformed mls message";
    case MlsError::Rejected:
        return "mls message rejected";
    case MlsError::NotAMember:
        return "not a member";
    case MlsError::Inactive:
        return "removed from the group";
    case MlsError::Internal:
        return "mls internal error";
    }
    return "unknown mls error";
}

void MlsClient::Free::operator()(UlwMlsClient* client) const noexcept {
    ulw_mls_client_free(client);
}

void MlsGroup::Free::operator()(UlwMlsGroup* group) const noexcept {
    ulw_mls_group_free(group);
}

MlsResult<MlsClient> MlsClient::create(std::span<const std::byte> identity) {
    UlwMlsClient* client = nullptr;
    if (const auto s = ulw_mls_client_new(data_of(identity), identity.size(), &client);
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return MlsClient{client};
}

MlsResult<MlsBytes> MlsClient::key_package() const {
    Buffer out;
    if (const auto s = ulw_mls_client_key_package(client_.get(), out.out());
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return out.take();
}

MlsResult<MlsGroup> MlsClient::create_group(std::span<const std::byte> group_id) const {
    UlwMlsGroup* group = nullptr;
    if (const auto s =
            ulw_mls_group_create(client_.get(), data_of(group_id), group_id.size(), &group);
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return MlsGroup{group};
}

MlsResult<MlsGroup> MlsClient::join(std::span<const std::byte> welcome) const {
    UlwMlsGroup* group = nullptr;
    if (const auto s = ulw_mls_group_join(client_.get(), data_of(welcome), welcome.size(), &group);
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return MlsGroup{group};
}

MlsResult<MlsAddition> MlsGroup::add(std::span<const MlsBytes> key_packages) {
    std::vector<UlwMlsBytes> views;
    views.reserve(key_packages.size());
    for (const MlsBytes& package : key_packages) {
        views.push_back(UlwMlsBytes{.data = data_of(package), .len = package.size()});
    }
    Buffer commit;
    Buffer welcome;
    if (const auto s = ulw_mls_group_add(group_.get(), views.data(), views.size(), commit.out(),
                                         welcome.out());
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return MlsAddition{.commit = commit.take(), .welcome = welcome.take()};
}

MlsResult<MlsBytes> MlsGroup::remove(std::span<const std::byte> identity) {
    Buffer commit;
    if (const auto s =
            ulw_mls_group_remove(group_.get(), data_of(identity), identity.size(), commit.out());
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return commit.take();
}

MlsResult<void> MlsGroup::merge_pending_commit() {
    if (const auto s = ulw_mls_group_merge_pending_commit(group_.get()); s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return {};
}

MlsResult<void> MlsGroup::clear_pending_commit() {
    if (const auto s = ulw_mls_group_clear_pending_commit(group_.get()); s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return {};
}

MlsResult<MlsBytes> MlsGroup::encrypt(std::span<const std::byte> plaintext) {
    Buffer out;
    if (const auto s =
            ulw_mls_group_encrypt(group_.get(), data_of(plaintext), plaintext.size(), out.out());
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return out.take();
}

MlsResult<MlsIncoming> MlsGroup::process(std::span<const std::byte> message) {
    UlwMlsReceived received = ULW_MLS_RECEIVED_APPLICATION;
    Buffer plaintext;
    if (const auto s = ulw_mls_group_process(group_.get(), data_of(message), message.size(),
                                             &received, plaintext.out());
        s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    MlsIncoming incoming{.kind = MlsReceived::Application, .plaintext = plaintext.take()};
    switch (received) {
    case ULW_MLS_RECEIVED_APPLICATION:
        break;
    case ULW_MLS_RECEIVED_COMMIT:
        incoming.kind = MlsReceived::Commit;
        break;
    case ULW_MLS_RECEIVED_PROPOSAL:
        incoming.kind = MlsReceived::Proposal;
        break;
    // The bridge writes this only on failure.
    case ULW_MLS_RECEIVED_NOTHING:
        return std::unexpected(MlsError::Internal);
    }
    return incoming;
}

MlsResult<std::uint64_t> MlsGroup::epoch() const {
    std::uint64_t epoch = 0;
    if (const auto s = ulw_mls_group_epoch(group_.get(), &epoch); s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return epoch;
}

MlsResult<std::size_t> MlsGroup::member_count() const {
    std::size_t count = 0;
    if (const auto s = ulw_mls_group_member_count(group_.get(), &count); s != ULW_MLS_STATUS_OK) {
        return std::unexpected(to_error(s));
    }
    return count;
}

} // namespace infra::e2ee
