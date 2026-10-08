#pragma once

#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"
#include "core/ports/catalog.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace gateway {

// The page sizes GET /api/v1/service/videos/{id}/grants answers (ADR-0097).
inline constexpr std::size_t kDefaultGrantPage = 100;
inline constexpr std::size_t kMaxGrantPage = 1000;

// A user id as a path segment carries it: percent-encoded or not, since clients encode the `|`
// of "provider|id" and some encode more. Anything that does not decode to a user id is nullopt.
[[nodiscard]] std::optional<core::UserId> user_from_segment(std::string_view segment) noexcept;

struct GrantQuery {
    std::optional<core::UserId> after;
    std::size_t limit = kDefaultGrantPage;
};

// `after` (a user id, percent-encoded or not) and `limit` (1 to kMaxGrantPage) from a request
// target's query, both optional. An unknown parameter is ignored; a malformed value, or one
// given twice, is nullopt.
[[nodiscard]] std::optional<GrantQuery> grant_query(std::string_view target) noexcept;

// PATCH /api/v1/videos/{id}'s body: {"visibility": "<private|unlisted|room:<room id>>"}.
[[nodiscard]] std::optional<core::Visibility> visibility_from_body(std::string_view body);

// The video state's name in the API: init, uploading, processing, ready or failed.
[[nodiscard]] std::string_view state_name(core::VideoState state) noexcept;

// The video object of GET /api/v1/videos/{id}. Only its owner sees its visibility and why it
// failed: a viewer learns what they can play, not whom else it is shared with.
[[nodiscard]] std::string video_json(const core::VideoRecord& video, core::VideoAccess access);

// {"video_id":..., "grants":[{"user_id":..., "granted_at":<unix seconds>}], "next":<cursor|null>}
[[nodiscard]] std::string grants_json(const core::VideoId& video,
                                      const core::ports::GrantPage& page);

// {"error":"<code>"}: the bodies of the access API's refusals.
[[nodiscard]] std::string error_json(std::string_view code);

} // namespace gateway
