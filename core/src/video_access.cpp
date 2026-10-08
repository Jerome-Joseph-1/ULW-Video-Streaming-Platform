#include "core/models/video_access.hpp"

#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/visibility.hpp"

namespace core {

namespace {

// The upload has been committed: there is a whole video to share, or there was one.
[[nodiscard]] bool past_upload(VideoState state) noexcept {
    switch (state) {
    case VideoState::Init:
    case VideoState::Uploading:
        return false;
    case VideoState::Processing:
    case VideoState::Ready:
    case VideoState::Failed:
        return true;
    }
    return false;
}

[[nodiscard]] bool shared_with(const Visibility& visibility, const ViewerFacts& facts) noexcept {
    switch (visibility.kind()) {
    case VisibilityKind::Private:
        return false;
    case VisibilityKind::Room:
        return facts.room_member;
    case VisibilityKind::Unlisted:
        return true;
    }
    return false;
}

} // namespace

VideoAccess access_of(const VideoRecord& video, const UserId& viewer,
                      const ViewerFacts& facts) noexcept {
    if (video.owner == viewer) {
        return VideoAccess::Owner;
    }
    if (!past_upload(video.state)) {
        return VideoAccess::None;
    }
    if (facts.granted || shared_with(video.visibility, facts)) {
        return VideoAccess::Viewer;
    }
    return VideoAccess::None;
}

} // namespace core
