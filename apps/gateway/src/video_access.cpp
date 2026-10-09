#include "video_access.hpp"

#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"
#include "core/ports/catalog.hpp"
#include "core/util/json.hpp"
#include "core/util/parse.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace gateway {

namespace {

[[nodiscard]] std::optional<unsigned> hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    return std::nullopt;
}

// Percent-decodes `text` into `out`; nullopt for a broken escape or more than `out` holds. '+'
// is itself: these are path segments and query values a client percent-encodes, not forms.
[[nodiscard]] std::optional<std::string_view>
percent_decode(std::string_view text, std::array<char, core::UserId::kMaxLength>& out) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (n == out.size()) {
            return std::nullopt;
        }
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) {
                return std::nullopt;
            }
            const auto high = hex_digit(text[i + 1]);
            const auto low = hex_digit(text[i + 2]);
            if (!high || !low) {
                return std::nullopt;
            }
            c = static_cast<char>((*high << 4U) | *low);
            i += 2;
        }
        out.at(n++) = c;
    }
    return std::string_view{out.data(), n};
}

[[nodiscard]] std::int64_t unix_seconds(core::WallTime t) noexcept {
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count();
}

} // namespace

std::optional<core::UserId> user_from_segment(std::string_view segment) noexcept {
    std::array<char, core::UserId::kMaxLength> buffer{};
    const auto text = percent_decode(segment, buffer);
    if (!text) {
        return std::nullopt;
    }
    auto user = core::UserId::parse(*text);
    if (!user) {
        return std::nullopt;
    }
    return *user;
}

std::optional<GrantQuery> grant_query(std::string_view target) noexcept {
    GrantQuery query;
    const std::size_t mark = target.find('?');
    if (mark == std::string_view::npos) {
        return query;
    }
    std::string_view rest = target.substr(mark + 1);
    rest = rest.substr(0, rest.find('#'));
    bool seen_after = false;
    bool seen_limit = false;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        const std::string_view name = pair.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
        if (name == "after") {
            auto user = user_from_segment(value);
            if (seen_after || !user) {
                return std::nullopt;
            }
            seen_after = true;
            query.after = *user;
        } else if (name == "limit") {
            const auto limit = core::parse_integer<std::size_t>(value);
            if (seen_limit || !limit || *limit == 0 || *limit > kMaxGrantPage) {
                return std::nullopt;
            }
            seen_limit = true;
            query.limit = *limit;
        }
    }
    return query;
}

std::optional<VideoQuery> video_query(std::string_view target) noexcept {
    const std::size_t mark = target.find('?');
    if (mark == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = target.substr(mark + 1);
    rest = rest.substr(0, rest.find('#'));
    std::optional<core::UserId> owner;
    std::optional<core::ports::VideoCursor> after;
    std::optional<std::size_t> limit;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        const std::string_view name = pair.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
        if (name == "owner") {
            auto user = user_from_segment(value);
            if (owner || !user) {
                return std::nullopt;
            }
            owner = *user;
        } else if (name == "after") {
            auto cursor = cursor_from_text(value);
            if (after || !cursor) {
                return std::nullopt;
            }
            after = *cursor;
        } else if (name == "limit") {
            const auto n = core::parse_integer<std::size_t>(value);
            if (limit || !n || *n == 0 || *n > kMaxVideoPage) {
                return std::nullopt;
            }
            limit = n;
        }
    }
    if (!owner) {
        return std::nullopt;
    }
    return VideoQuery{.owner = *owner, .after = after, .limit = limit.value_or(kDefaultVideoPage)};
}

std::string cursor_text(const core::ports::VideoCursor& cursor) {
    return std::format("{}.{}", cursor.created_at_us, cursor.id.to_string());
}

std::optional<core::ports::VideoCursor> cursor_from_text(std::string_view text) noexcept {
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto created = core::parse_integer<std::int64_t>(text.substr(0, dot));
    const auto id = core::VideoId::parse(text.substr(dot + 1));
    if (!created || *created < 0 || !id) {
        return std::nullopt;
    }
    return core::ports::VideoCursor{.created_at_us = *created, .id = *id};
}

std::optional<core::Visibility> visibility_from_body(std::string_view body) {
    const auto doc = core::json::parse(body);
    if (!doc || doc->as_object() == nullptr) {
        return std::nullopt;
    }
    const core::json::Value* field = doc->find("visibility");
    const std::optional<std::string_view> text =
        field != nullptr ? field->as_string() : std::nullopt;
    if (!text) {
        return std::nullopt;
    }
    auto visibility = core::Visibility::parse(*text);
    if (!visibility) {
        return std::nullopt;
    }
    return *visibility;
}

std::string_view state_name(core::VideoState state) noexcept {
    switch (state) {
    case core::VideoState::Init:
        return "init";
    case core::VideoState::Uploading:
        return "uploading";
    case core::VideoState::Processing:
        return "processing";
    case core::VideoState::Ready:
        return "ready";
    case core::VideoState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view stage_name(core::TranscodeStage stage) noexcept {
    switch (stage) {
    case core::TranscodeStage::Queued:
        return "queued";
    case core::TranscodeStage::Transcoding:
        return "transcoding";
    }
    return "unknown";
}

std::string video_json(const core::VideoRecord& video, core::VideoAccess access) {
    std::string json = R"({"id":")" + video.id.to_string() + R"(","title":)";
    core::json::append_string(json, video.title);
    json += std::format(R"(,"state":"{}","version":{},"duration_ms":)", state_name(video.state),
                        video.version);
    json += video.duration ? std::to_string(video.duration->count()) : "null";
    // Whoever may see the video may see how far along it is (ADR-0101).
    json += R"(,"progress":)";
    if (video.state == core::VideoState::Processing && video.progress) {
        json += std::format(R"({{"stage":"{}","percent":{}}})", stage_name(video.progress->stage),
                            video.progress->percent);
    } else {
        json += "null";
    }
    if (access == core::VideoAccess::Owner) {
        // Only a failed video has one, and it is written for its owner (the worker's
        // public_reason, the reaper's "upload expired"); the details stay in the logs.
        if (video.state == core::VideoState::Failed && video.error_reason) {
            json += R"(,"error_reason":)";
            core::json::append_string(json, *video.error_reason);
        }
        json += R"(,"visibility":")" + video.visibility.to_string() + '"';
    }
    json += '}';
    return json;
}

std::string grants_json(const core::VideoId& video, const core::ports::GrantPage& page) {
    std::string json = R"({"video_id":")" + video.to_string() + R"(","grants":[)";
    bool first = true;
    for (const core::ports::VideoGrant& grant : page.grants) {
        json += first ? "" : ",";
        first = false;
        json += R"({"user_id":)";
        core::json::append_string(json, grant.user.view());
        json += std::format(R"(,"granted_at":{}}})", unix_seconds(grant.granted_at));
    }
    json += R"(],"next":)";
    if (page.more && !page.grants.empty()) {
        core::json::append_string(json, page.grants.back().user.view());
    } else {
        json += "null";
    }
    json += '}';
    return json;
}

std::string videos_json(const core::UserId& owner, const core::ports::VideoPage& page) {
    std::string json = R"({"owner":)";
    core::json::append_string(json, owner.view());
    json += R"(,"videos":[)";
    bool first = true;
    for (const core::ports::ListedVideo& listed : page.videos) {
        json += first ? "" : ",";
        first = false;
        // The object GET /api/v1/videos/{id} answers its owner, and when it was created.
        std::string video = video_json(listed.video, core::VideoAccess::Owner);
        video.pop_back();
        json += video;
        json += std::format(R"(,"created_at":{}}})", listed.created_at_us / 1'000'000);
    }
    json += R"(],"next":)";
    if (page.more && !page.videos.empty()) {
        const core::ports::ListedVideo& last = page.videos.back();
        core::json::append_string(
            json, cursor_text({.created_at_us = last.created_at_us, .id = last.video.id}));
    } else {
        json += "null";
    }
    json += '}';
    return json;
}

std::string error_json(std::string_view code) {
    std::string json = R"({"error":)";
    core::json::append_string(json, code);
    json += '}';
    return json;
}

} // namespace gateway
