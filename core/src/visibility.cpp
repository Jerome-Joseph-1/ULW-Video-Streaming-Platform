#include "core/models/visibility.hpp"

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace core {

namespace {

constexpr std::string_view kPrivate = "private";
constexpr std::string_view kRoom = "room";
constexpr std::string_view kUnlisted = "unlisted";
constexpr std::string_view kRoomPrefix = "room:";

} // namespace

std::expected<Visibility, DomainError> Visibility::parse(std::string_view text) noexcept {
    if (text == kPrivate) {
        return Visibility{};
    }
    if (text == kUnlisted) {
        return unlisted();
    }
    if (!text.starts_with(kRoomPrefix)) {
        return std::unexpected(DomainError::InvalidVisibility);
    }
    const auto id = RoomId::parse(text.substr(kRoomPrefix.size()));
    if (!id) {
        return std::unexpected(DomainError::InvalidVisibility);
    }
    return room(*id);
}

std::expected<Visibility, DomainError>
Visibility::from_columns(std::string_view kind,
                         std::optional<std::string_view> room_text) noexcept {
    if (kind == kRoom) {
        if (!room_text) {
            return std::unexpected(DomainError::InvalidVisibility);
        }
        const auto id = RoomId::parse(*room_text);
        if (!id) {
            return std::unexpected(DomainError::InvalidVisibility);
        }
        return room(*id);
    }
    // Only a room's kind names a room; anything else with one is not a row the API wrote.
    if (room_text) {
        return std::unexpected(DomainError::InvalidVisibility);
    }
    if (kind == kPrivate) {
        return Visibility{};
    }
    if (kind == kUnlisted) {
        return unlisted();
    }
    return std::unexpected(DomainError::InvalidVisibility);
}

std::string_view Visibility::kind_name() const noexcept {
    switch (kind_) {
    case VisibilityKind::Private:
        return kPrivate;
    case VisibilityKind::Room:
        return kRoom;
    case VisibilityKind::Unlisted:
        return kUnlisted;
    }
    return kPrivate;
}

std::string Visibility::to_string() const {
    if (kind_ == VisibilityKind::Room && room_) {
        return std::string(kRoomPrefix) + room_->to_string();
    }
    return std::string(kind_name());
}

} // namespace core
