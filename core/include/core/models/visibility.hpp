#pragma once

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace core {

// Who besides its owner may see a video (ADR-0097). Explicit grants come on top of any of them.
enum class VisibilityKind : std::uint8_t {
    // The owner alone: every video starts so.
    Private,
    // The current members of one chat room, as chat_members lists them at each request.
    Room,
    // Any signed-in user who has the video's id.
    Unlisted,
};

class Visibility {
public:
    // "room:" and a canonical UUID: the longest text form.
    static constexpr std::size_t kMaxTextLength = 5 + Uuid::kTextLength;

    // Private.
    Visibility() = default;

    [[nodiscard]] static Visibility unlisted() noexcept {
        return Visibility{VisibilityKind::Unlisted, std::nullopt};
    }
    [[nodiscard]] static Visibility room(const RoomId& room) noexcept {
        return Visibility{VisibilityKind::Room, room};
    }

    // "private", "unlisted" or "room:<room id>", the room id a lowercase canonical UUID: what
    // the API takes and answers.
    [[nodiscard]] static std::expected<Visibility, DomainError>
    parse(std::string_view text) noexcept;
    // The catalog's two columns: the kind's name, and the room for a room's kind only.
    [[nodiscard]] static std::expected<Visibility, DomainError>
    from_columns(std::string_view kind, std::optional<std::string_view> room) noexcept;

    [[nodiscard]] VisibilityKind kind() const noexcept { return kind_; }
    // Set exactly when the kind is Room.
    [[nodiscard]] const std::optional<RoomId>& room_id() const noexcept { return room_; }
    // "private", "room" or "unlisted": the catalog's kind column.
    [[nodiscard]] std::string_view kind_name() const noexcept;
    // The form parse() takes.
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const Visibility&, const Visibility&) = default;

private:
    Visibility(VisibilityKind kind, std::optional<RoomId> room) noexcept
        : kind_(kind), room_(room) {}

    VisibilityKind kind_ = VisibilityKind::Private;
    std::optional<RoomId> room_;
};

} // namespace core
