#include "upload_claim.hpp"

namespace gateway {

UploadClaim::~UploadClaim() {
    release_any();
}

void UploadClaim::adopt(std::uint64_t request, const core::UploadId& upload,
                        core::ports::ClaimToken token) noexcept {
    release_any();
    holder_ = Holder{.request = request, .upload = upload, .token = token};
    ++held_;
}

void UploadClaim::release(std::uint64_t request) noexcept {
    if (held_by(request)) {
        release_any();
    }
}

void UploadClaim::release_any() noexcept {
    if (!holder_) {
        return;
    }
    catalog_.release_upload(holder_->upload, holder_->token);
    holder_.reset();
    --held_;
}

} // namespace gateway
