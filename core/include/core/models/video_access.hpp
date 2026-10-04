#pragma once

#include "core/models/ids.hpp"
#include "core/models/video.hpp"

#include <cstdint>

namespace core {

// What the catalog found about one viewer of one video, in the same read as the video itself
// (ADR-0097): whether chat_members lists them in the room the video is shared with, and whether
// the operator's backend granted them the video.
struct ViewerFacts {
    bool room_member = false;
    bool granted = false;
};

enum class VideoAccess : std::uint8_t {
    // Answered exactly as a video that does not exist: 404, nothing about it.
    None,
    // May see its metadata and play it, and nothing more.
    Viewer,
    // Everything, its visibility and its failure reason included.
    Owner,
};

// The one rule every path that serves a video goes through:
//   - its owner always;
//   - nobody else while its upload is in progress (init, uploading): what exists of it is the
//     uploader's alone, whatever its visibility or grants say;
//   - then anyone it is unlisted for, any current member of the room it is shared with, and any
//     user granted it.
[[nodiscard]] VideoAccess access_of(const VideoRecord& video, const UserId& viewer,
                                    const ViewerFacts& facts) noexcept;

} // namespace core
