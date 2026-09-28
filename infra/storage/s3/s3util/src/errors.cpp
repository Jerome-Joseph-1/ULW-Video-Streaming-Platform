#include "infra/s3util/errors.hpp"

#include "core/ports/storage.hpp"

#include <string_view>

namespace infra::s3util {

MappedError map_error(int http_status, std::string_view s3_error_code) noexcept {
    using core::ports::StorageError;
    const auto is = [s3_error_code](std::string_view code) { return s3_error_code == code; };
    // Codes are checked before statuses: several arrive with a status that says something
    // else (SignatureDoesNotMatch as 403, SlowDown as 503, anything at all inside a 200).
    if (is("SignatureDoesNotMatch")) {
        return {.error = StorageError::Permanent, .page = true};
    }
    // Bad or expired credentials and a skewed clock fail every request alike until someone
    // fixes the deployment.
    if (is("InvalidAccessKeyId") || is("ExpiredToken") || is("RequestTimeTooSkewed")) {
        return {.error = StorageError::Unauthorized, .page = true};
    }
    // Not NotFound: a misconfigured bucket would otherwise look like every object missing.
    if (is("NoSuchBucket")) {
        return {.error = StorageError::Permanent, .page = true};
    }
    if (is("NoSuchUpload") || is("NoSuchKey") || http_status == 404) {
        return {.error = StorageError::NotFound, .page = false};
    }
    if (is("AccessDenied") || http_status == 403) {
        return {.error = StorageError::Unauthorized, .page = false};
    }
    if (http_status == 412) {
        return {.error = StorageError::PreconditionFailed, .page = false};
    }
    if (is("SlowDown") || http_status == 429) {
        return {.error = StorageError::Throttled, .page = false};
    }
    if (is("BadDigest")) {
        return {.error = StorageError::Corrupt, .page = false};
    }
    // InternalError and ServiceUnavailable are the codes of a 5xx, and they also turn up in
    // the 200 of a completion or copy that failed late. RequestTimeout is a 400 for an upload
    // body that stalled, which a fresh attempt usually gets through. OperationAborted is a 409
    // for a conflicting operation still in flight on the same key, which S3 says to retry.
    if ((http_status >= 500 && http_status <= 599) || is("InternalError") ||
        is("ServiceUnavailable") || is("RequestTimeout") || is("OperationAborted")) {
        return {.error = StorageError::Transient, .page = false};
    }
    return {.error = StorageError::Permanent, .page = false};
}

} // namespace infra::s3util
