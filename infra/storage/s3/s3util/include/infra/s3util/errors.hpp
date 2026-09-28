#pragma once

#include "core/ports/storage.hpp"

#include <string_view>

namespace infra::s3util {

struct MappedError {
    core::ports::StorageError error = core::ports::StorageError::Permanent;
    // The request or the deployment was wrong in a way only a fix on our side can cure (a bad
    // signature, credentials, clock or bucket): retrying will not help and the client cannot
    // act on it, so a person has to look.
    bool page = false;
};

// `s3_error_code` is the <Code> of the error document, or empty when there was none (a HEAD
// response, or a body that did not parse). A 2xx status with a code is an error that S3
// reported inside a success response.
[[nodiscard]] MappedError map_error(int http_status, std::string_view s3_error_code) noexcept;

} // namespace infra::s3util
