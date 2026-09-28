#include "core/ports/storage.hpp"
#include "infra/s3util/errors.hpp"

#include <array>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using core::ports::StorageError;
using infra::s3util::map_error;

struct Case {
    int status;
    std::string_view code;
    StorageError error;
    bool page;
};

constexpr Case row(int status, std::string_view code, StorageError error, bool page = false) {
    return Case{.status = status, .code = code, .error = error, .page = page};
}

using enum StorageError;

constexpr std::array kTable{
    row(404, "", NotFound),
    row(404, "NoSuchKey", NotFound),
    row(404, "NoSuchUpload", NotFound),
    // A missing bucket is our configuration, not a missing object.
    row(404, "NoSuchBucket", Permanent, true),
    row(200, "NoSuchUpload", NotFound),
    row(403, "AccessDenied", Unauthorized),
    row(403, "", Unauthorized),
    row(403, "InvalidAccessKeyId", Unauthorized, true),
    row(400, "ExpiredToken", Unauthorized, true),
    row(403, "ExpiredToken", Unauthorized, true),
    row(403, "RequestTimeTooSkewed", Unauthorized, true),
    row(403, "SignatureDoesNotMatch", Permanent, true),
    row(412, "PreconditionFailed", PreconditionFailed),
    row(412, "", PreconditionFailed),
    row(429, "", Throttled),
    row(503, "SlowDown", Throttled),
    row(200, "SlowDown", Throttled),
    row(500, "InternalError", Transient),
    row(500, "", Transient),
    row(502, "", Transient),
    row(503, "ServiceUnavailable", Transient),
    row(599, "", Transient),
    row(200, "InternalError", Transient),
    row(400, "RequestTimeout", Transient),
    row(409, "OperationAborted", Transient),
    row(400, "BadDigest", Corrupt),
    row(400, "InvalidArgument", Permanent),
    row(400, "EntityTooSmall", Permanent),
    row(400, "InvalidPart", Permanent),
    row(400, "InvalidPartOrder", Permanent),
    row(400, "", Permanent),
    row(405, "MethodNotAllowed", Permanent),
    row(409, "", Permanent),
    row(499, "", Permanent),
    row(600, "", Permanent),
};

TEST(MapError, ClassifiesEveryRowOfTheTable) {
    for (const auto& c : kTable) {
        const auto mapped = map_error(c.status, c.code);
        EXPECT_EQ(mapped.error, c.error) << c.status << ' ' << c.code;
        EXPECT_EQ(mapped.page, c.page) << c.status << ' ' << c.code;
    }
}

} // namespace
