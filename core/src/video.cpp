#include "core/models/video.hpp"

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace core {

namespace {

struct ByteRange {
    unsigned char min;
    unsigned char max;
};

constexpr bool contains(ByteRange range, char c) noexcept {
    const auto b = static_cast<unsigned char>(c);
    return b >= range.min && b <= range.max;
}

// Well-formed UTF-8 byte sequences, Unicode Standard table 3-7. The narrowed second-byte ranges
// exclude overlong encodings, UTF-16 surrogates and code points above U+10FFFF.
struct Utf8Row {
    ByteRange lead;
    std::size_t length;
    ByteRange second;
};

constexpr ByteRange kContinuation{.min = 0x80, .max = 0xBF};

constexpr std::array<Utf8Row, 8> kUtf8Rows{{
    {.lead = {.min = 0xC2, .max = 0xDF}, .length = 2, .second = kContinuation},
    {.lead = {.min = 0xE0, .max = 0xE0}, .length = 3, .second = {.min = 0xA0, .max = 0xBF}},
    {.lead = {.min = 0xE1, .max = 0xEC}, .length = 3, .second = kContinuation},
    {.lead = {.min = 0xED, .max = 0xED}, .length = 3, .second = {.min = 0x80, .max = 0x9F}},
    {.lead = {.min = 0xEE, .max = 0xEF}, .length = 3, .second = kContinuation},
    {.lead = {.min = 0xF0, .max = 0xF0}, .length = 4, .second = {.min = 0x90, .max = 0xBF}},
    {.lead = {.min = 0xF1, .max = 0xF3}, .length = 4, .second = kContinuation},
    {.lead = {.min = 0xF4, .max = 0xF4}, .length = 4, .second = {.min = 0x80, .max = 0x8F}},
}};

// Length of the well-formed sequence that starts `text`, or 0 if it does not start with one.
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text) noexcept {
    if (static_cast<unsigned char>(text.front()) < 0x80U) {
        return 1;
    }
    for (const Utf8Row& row : kUtf8Rows) {
        if (!contains(row.lead, text.front())) {
            continue;
        }
        const bool well_formed = text.size() >= row.length && contains(row.second, text[1]) &&
                                 std::ranges::all_of(text.substr(2, row.length - 2), [](char c) {
                                     return contains(kContinuation, c);
                                 });
        return well_formed ? row.length : 0;
    }
    return 0;
}

// Titles and failure reasons are echoed verbatim into JSON and stored in a UTF-8 text column, so
// malformed text is rejected here instead of failing later on the way out.
[[nodiscard]] bool is_valid_text(std::string_view text, std::size_t max_bytes) noexcept {
    if (text.empty() || text.size() > max_bytes) {
        return false;
    }
    while (!text.empty()) {
        // Every byte of a multi-byte sequence is 0x80 or above, so testing lead bytes catches
        // every ASCII control character.
        const auto lead = static_cast<unsigned char>(text.front());
        if (lead < 0x20U || lead == 0x7FU) {
            return false;
        }
        const std::size_t length = utf8_sequence_length(text);
        if (length == 0) {
            return false;
        }
        text.remove_prefix(length);
    }
    return true;
}

// Every transition bumps the version: leaving Init takes at least one, and reaching Ready at
// least two (Init -> Processing -> Ready).
std::uint64_t lowest_reachable_version(VideoState state) noexcept {
    switch (state) {
    case VideoState::Init:
        return 0;
    case VideoState::Uploading:
    case VideoState::Processing:
    case VideoState::Failed:
        return 1;
    case VideoState::Ready:
        return 2;
    }
    return std::numeric_limits<std::uint64_t>::max();
}

bool is_terminal(VideoState state) noexcept {
    return state == VideoState::Ready || state == VideoState::Failed;
}

[[nodiscard]] std::expected<void, DomainError> validate(const VideoRecord& record) noexcept {
    if (!is_valid_text(record.title, Video::kMaxTitleBytes)) {
        return std::unexpected(DomainError::InvalidTitle);
    }
    const bool failed = record.state == VideoState::Failed;
    const bool ready = record.state == VideoState::Ready;
    if (failed && (!record.error_reason || record.error_reason->empty())) {
        return std::unexpected(DomainError::MissingFailureReason);
    }
    if (record.duration && *record.duration < Millis::zero()) {
        return std::unexpected(DomainError::InvalidDuration);
    }
    if (ready != record.duration.has_value() || (!failed && record.error_reason)) {
        return std::unexpected(DomainError::CorruptRecord);
    }
    if (failed && !is_valid_text(*record.error_reason, Video::kMaxFailureReasonBytes)) {
        return std::unexpected(DomainError::InvalidFailureReason);
    }
    // At the maximum the next bump would wrap and defeat the repository's version check.
    if (record.version < lowest_reachable_version(record.state) ||
        record.version == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(DomainError::CorruptRecord);
    }
    return {};
}

} // namespace

std::expected<Video, DomainError> Video::create(VideoId id, const UserId& owner,
                                                std::string title) {
    VideoRecord record{.id = id,
                       .owner = owner,
                       .title = std::move(title),
                       .state = VideoState::Init,
                       .version = 0,
                       .error_reason = std::nullopt,
                       .duration = std::nullopt};
    if (const auto ok = validate(record); !ok) {
        return std::unexpected(ok.error());
    }
    return Video{std::move(record)};
}

std::expected<Video, DomainError> Video::rehydrate(const VideoRecord& record) {
    if (const auto ok = validate(record); !ok) {
        return std::unexpected(ok.error());
    }
    return Video{record};
}

std::expected<void, DomainError> Video::start_upload() noexcept {
    if (const auto ok = require_state({VideoState::Init}); !ok) {
        return ok;
    }
    enter(VideoState::Uploading);
    return {};
}

std::expected<void, DomainError> Video::start_processing() noexcept {
    if (const auto ok = require_state({VideoState::Init, VideoState::Uploading}); !ok) {
        return ok;
    }
    enter(VideoState::Processing);
    return {};
}

std::expected<void, DomainError> Video::mark_ready(Millis duration) noexcept {
    if (const auto ok = require_state({VideoState::Processing}); !ok) {
        return ok;
    }
    if (duration < Millis::zero()) {
        return std::unexpected(DomainError::InvalidDuration);
    }
    data_.duration = duration;
    enter(VideoState::Ready);
    return {};
}

std::expected<void, DomainError> Video::mark_failed(std::string reason) noexcept {
    if (const auto ok =
            require_state({VideoState::Init, VideoState::Uploading, VideoState::Processing});
        !ok) {
        return ok;
    }
    if (reason.empty()) {
        return std::unexpected(DomainError::MissingFailureReason);
    }
    if (!is_valid_text(reason, kMaxFailureReasonBytes)) {
        return std::unexpected(DomainError::InvalidFailureReason);
    }
    data_.error_reason = std::move(reason);
    enter(VideoState::Failed);
    return {};
}

std::optional<std::string_view> Video::error_reason() const noexcept {
    if (!data_.error_reason) {
        return std::nullopt;
    }
    return *data_.error_reason;
}

// AlreadyTerminal alone cannot tell a redelivered request from a conflicting one; the caller
// compares state() with the state it asked for.
std::expected<void, DomainError>
Video::require_state(std::initializer_list<VideoState> allowed) const noexcept {
    if (is_terminal(data_.state)) {
        return std::unexpected(DomainError::AlreadyTerminal);
    }
    if (std::ranges::find(allowed, data_.state) == allowed.end()) {
        return std::unexpected(DomainError::InvalidTransition);
    }
    return {};
}

void Video::enter(VideoState next) noexcept {
    data_.state = next;
    ++data_.version;
}

} // namespace core
