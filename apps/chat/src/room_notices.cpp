#include "room_notices.hpp"

#include "layout.hpp"

#include <chrono>

namespace chat {

namespace {

// After the ring's notice layout (1).
constexpr std::uint8_t kActivityLayout = 2;
constexpr std::uint8_t kTypingLayout = 3;
constexpr std::uint8_t kReadLayout = 4;
constexpr std::uint8_t kRedactionLayout = 5;

std::uint64_t unix_millis(core::WallTime at) noexcept {
    const auto ms = std::chrono::floor<core::Millis>(at.time_since_epoch()).count();
    return ms < 0 ? 0 : static_cast<std::uint64_t>(ms);
}

core::WallTime from_unix_millis(std::uint64_t ms) noexcept {
    return core::WallTime{core::Millis{static_cast<core::Millis::rep>(ms)}};
}

} // namespace

std::vector<std::byte> encode_activity(const ActivityNotice& notice) {
    std::vector<std::byte> out;
    layout::put_u8(out, kActivityLayout);
    layout::put_short(out, notice.to.view());
    layout::put_uuid(out, notice.room);
    layout::put_u64(out, notice.seq);
    layout::put_short(out, notice.sender.view());
    layout::put_u64(out, unix_millis(notice.at));
    layout::put_u8(out, notice.quiet ? 1 : 0);
    return out;
}

std::optional<ActivityNotice> decode_activity(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    if (in.u8() != kActivityLayout) {
        return std::nullopt;
    }
    auto to = in.user();
    const auto room = in.uuid<core::RoomId>();
    const auto seq = in.u64();
    auto sender = in.user();
    const auto at = in.u64();
    const auto quiet = in.u8();
    if (!to || !room || !seq || !sender || !at || !quiet || *quiet > 1 || !in.empty()) {
        return std::nullopt;
    }
    return ActivityNotice{.to = std::move(*to),
                          .room = *room,
                          .seq = *seq,
                          .sender = std::move(*sender),
                          .at = from_unix_millis(*at),
                          .quiet = *quiet == 1};
}

bool is_activity(std::span<const std::byte> bytes) noexcept {
    return !bytes.empty() && std::to_integer<std::uint8_t>(bytes.front()) == kActivityLayout;
}

std::vector<std::byte> encode_typing(const TypingNotice& notice) {
    std::vector<std::byte> out;
    layout::put_u8(out, kTypingLayout);
    layout::put_uuid(out, notice.room);
    layout::put_short(out, notice.user.view());
    return out;
}

std::vector<std::byte> encode_read(const ReadNotice& notice) {
    std::vector<std::byte> out;
    layout::put_u8(out, kReadLayout);
    layout::put_uuid(out, notice.room);
    layout::put_short(out, notice.user.view());
    layout::put_u64(out, notice.seq);
    return out;
}

std::vector<std::byte> encode_redaction(const RedactionNotice& notice) {
    std::vector<std::byte> out;
    layout::put_u8(out, kRedactionLayout);
    layout::put_uuid(out, notice.room);
    layout::put_u64(out, notice.seq);
    return out;
}

std::optional<RoomNotice> decode_room_notice(std::span<const std::byte> bytes) {
    layout::Reader in{bytes};
    const auto kind = in.u8();
    // A redaction names no user: its room and the seq blanked.
    if (kind == kRedactionLayout) {
        const auto room = in.uuid<core::RoomId>();
        const auto seq = in.u64();
        if (!room || !seq || !in.empty()) {
            return std::nullopt;
        }
        return RedactionNotice{.room = *room, .seq = *seq};
    }
    if (kind != kTypingLayout && kind != kReadLayout) {
        return std::nullopt;
    }
    const auto room = in.uuid<core::RoomId>();
    auto user = in.user();
    if (!room || !user) {
        return std::nullopt;
    }
    if (kind == kTypingLayout) {
        if (!in.empty()) {
            return std::nullopt;
        }
        return TypingNotice{.room = *room, .user = std::move(*user)};
    }
    const auto seq = in.u64();
    if (!seq || !in.empty()) {
        return std::nullopt;
    }
    return ReadNotice{.room = *room, .user = std::move(*user), .seq = *seq};
}

} // namespace chat
