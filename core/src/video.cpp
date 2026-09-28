#include "core/models/video.hpp"

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
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
std::size_t utf8_sequence_length(std::string_view text) noexcept {
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

// Titles are echoed verbatim into JSON and stored in a UTF-8 text column, so a malformed one is
// rejected here as the client's error instead of failing later on the way out.
bool is_valid_title(std::string_view title) noexcept {
    if (title.empty() || title.size() > Video::kMaxTitleBytes) {
        return false;
    }
    while (!title.empty()) {
        // Every byte of a multi-byte sequence is 0x80 or above, so testing lead bytes catches
        // every ASCII control character.
        const auto lead = static_cast<unsigned char>(title.front());
        if (lead < 0x20U || lead == 0x7FU) {
            return false;
        }
        const std::size_t length = utf8_sequence_length(title);
        if (length == 0) {
            return false;
        }
        title.remove_prefix(length);
    }
    return true;
}

bool is_terminal(VideoState state) noexcept {
    return state == VideoState::Ready || state == VideoState::Failed;
}

std::expected<void, DomainError> validate(const VideoRecord& record) noexcept {
    if (!is_valid_title(record.title)) {
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
    if (auto ok = require_state({VideoState::Init}); !ok) {
        return ok;
    }
    enter(VideoState::Uploading);
    return {};
}

std::expected<void, DomainError> Video::start_processing() noexcept {
    if (auto ok = require_state({VideoState::Init, VideoState::Uploading}); !ok) {
        return ok;
    }
    enter(VideoState::Processing);
    return {};
}

std::expected<void, DomainError> Video::mark_ready(Millis duration) noexcept {
    if (auto ok = require_state({VideoState::Processing}); !ok) {
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
    if (auto ok = require_state({VideoState::Init, VideoState::Uploading, VideoState::Processing});
        !ok) {
        return ok;
    }
    if (reason.empty()) {
        return std::unexpected(DomainError::MissingFailureReason);
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

// Terminal states answer with their own error so a redelivered worker message can be
// acknowledged as already applied rather than treated as an ordering bug.
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
