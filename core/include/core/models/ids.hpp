#pragma once

#include "core/errors/domain_error.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/util/uuid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace core {

// One instantiation per entity, so a VideoId cannot be passed where an UploadId is expected.
template <typename Tag> class UuidId {
public:
    [[nodiscard]] static std::expected<UuidId, DomainError> parse(std::string_view text) noexcept {
        if (text.empty()) {
            return std::unexpected(DomainError::EmptyIdentifier);
        }
        const std::optional<Uuid> uuid = Uuid::parse(text);
        if (!uuid) {
            return std::unexpected(DomainError::MalformedIdentifier);
        }
        return UuidId{*uuid};
    }

    [[nodiscard]] static UuidId generate(const ports::IClock& clock,
                                         ports::IRandom& random) noexcept {
        return UuidId{Uuid::v7(clock, random)};
    }

    [[nodiscard]] const Uuid& uuid() const noexcept { return uuid_; }
    void format_to(std::span<char, Uuid::kTextLength> out) const noexcept { uuid_.format_to(out); }
    [[nodiscard]] std::string to_string() const { return uuid_.to_string(); }

    friend bool operator==(const UuidId&, const UuidId&) = default;
    friend auto operator<=>(const UuidId&, const UuidId&) = default;

private:
    explicit UuidId(const Uuid& uuid) noexcept : uuid_(uuid) {}

    Uuid uuid_;
};

using VideoId = UuidId<struct VideoTag>;
using UploadId = UuidId<struct UploadTag>;
using RoomId = UuidId<struct RoomTag>;
using DeviceId = UuidId<struct DeviceTag>;

// The `sub` claim of the external identity provider, verbatim; there is no local user table to
// mint ids from. Stored inline because every authenticated request parses one.
class UserId {
public:
    // OIDC allows 255 ASCII characters, but mainstream providers issue under 50 (Okta 20, Auth0
    // about 35, Entra ID 43, Apple 44). 128 keeps ample headroom and halves what a token can make
    // us store and index per row.
    static constexpr std::size_t kMaxLength = 128;

    // [A-Za-z0-9._:@|+-] covers those formats (Auth0's "provider|id", URN-like "a:b", e-mail
    // shaped subjects) and nothing that needs quoting in a log line or escaping in JSON.
    [[nodiscard]] static std::expected<UserId, DomainError> parse(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const noexcept { return {chars_.data(), size_}; }

    friend bool operator==(const UserId& a, const UserId& b) noexcept {
        return a.view() == b.view();
    }

private:
    UserId() = default;

    std::array<char, kMaxLength> chars_{};
    std::uint8_t size_ = 0;
    static_assert(kMaxLength <= std::numeric_limits<std::uint8_t>::max());
};

// A node's name, taken from its pod hostname. Kubernetes forms that hostname by truncating the pod
// name to 63 characters, which makes it an RFC 1123 label: [a-z0-9-], no leading or trailing '-'.
class NodeId {
public:
    static constexpr std::size_t kMaxLength = 63;

    [[nodiscard]] static std::expected<NodeId, DomainError> parse(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const noexcept { return {chars_.data(), size_}; }

    friend bool operator==(const NodeId& a, const NodeId& b) noexcept {
        return a.view() == b.view();
    }

private:
    NodeId() = default;

    std::array<char, kMaxLength> chars_{};
    std::uint8_t size_ = 0;
    static_assert(kMaxLength <= std::numeric_limits<std::uint8_t>::max());
};

} // namespace core

// [namespace.std] allows partial specialisations over program-defined types; the check only
// recognises full ones.
// NOLINTNEXTLINE(cert-dcl58-cpp)
template <typename Tag> struct std::hash<core::UuidId<Tag>> {
    [[nodiscard]] std::size_t operator()(const core::UuidId<Tag>& id) const noexcept {
        return std::hash<core::Uuid>{}(id.uuid());
    }
};

template <> struct std::hash<core::UserId> {
    [[nodiscard]] std::size_t operator()(const core::UserId& id) const noexcept {
        return std::hash<std::string_view>{}(id.view());
    }
};

template <> struct std::hash<core::NodeId> {
    [[nodiscard]] std::size_t operator()(const core::NodeId& id) const noexcept {
        return std::hash<std::string_view>{}(id.view());
    }
};
