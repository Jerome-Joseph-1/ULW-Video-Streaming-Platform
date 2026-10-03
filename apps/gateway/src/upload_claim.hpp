#pragma once

#include "core/models/ids.hpp"
#include "core/ports/catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace gateway {

// The claim on an upload's appends (ADR-0065) that one request on a connection holds, and the
// request that holds it. A connection serves one request at a time and a request claims at most
// one upload, so this holds at most one claim. Every release names the request it is for, by
// the number the connection gave that request: a completion that arrives after its request has
// ended carries a number that is no longer current, and can release nothing a later request
// holds. The release that reaches the catalog names the grant too (ClaimToken), so it cannot
// give back a later grant of the same upload either. Whatever is still held when this goes is
// released then.
//
// `held` is the shard's count of claims held, which /metrics reports as catalog_claims_held.
class UploadClaim {
public:
    UploadClaim(core::ports::IUploadCatalog& catalog, std::size_t& held) noexcept
        : catalog_(catalog), held_(held) {}
    ~UploadClaim();
    UploadClaim(const UploadClaim&) = delete;
    UploadClaim& operator=(const UploadClaim&) = delete;
    UploadClaim(UploadClaim&&) = delete;
    UploadClaim& operator=(UploadClaim&&) = delete;

    // The catalog granted `request` its claim on `upload`, the grant `token` names. A claim
    // still held by another request is released first: one request's claim never outlives the
    // next one's start.
    void adopt(std::uint64_t request, const core::UploadId& upload,
               core::ports::ClaimToken token) noexcept;
    // Releases the claim if `request` holds it. Any other request's claim is left as it is.
    void release(std::uint64_t request) noexcept;
    // Releases whatever is held, whoever holds it: the connection is closing.
    void release_any() noexcept;

    [[nodiscard]] bool held() const noexcept { return holder_.has_value(); }
    [[nodiscard]] bool held_by(std::uint64_t request) const noexcept {
        return holder_ && holder_->request == request;
    }
    // The grant `request` holds, for the catalog calls made under it; nullopt when it holds none.
    [[nodiscard]] std::optional<core::ports::ClaimToken>
    token_of(std::uint64_t request) const noexcept {
        if (holder_ && holder_->request == request) {
            return holder_->token;
        }
        return std::nullopt;
    }

private:
    struct Holder {
        std::uint64_t request;
        core::UploadId upload;
        // Released by this, so a release never reaches a later grant of the same upload.
        core::ports::ClaimToken token;
    };

    core::ports::IUploadCatalog& catalog_;
    std::size_t& held_;
    std::optional<Holder> holder_;
};

} // namespace gateway
