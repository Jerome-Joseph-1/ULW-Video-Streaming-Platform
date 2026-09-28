#pragma once

#include "core/ports/storage.hpp"
#include "core/util/time.hpp"
#include "infra/curl/http.hpp"
#include "infra/s3util/xml.hpp"

#include <optional>

namespace infra::storage::s3 {

// Why one attempt at a request did not produce what was asked for.
struct Failed {
    core::ports::StorageError error = core::ports::StorageError::Permanent;
    // Honoured as a floor by the retry policy.
    std::optional<core::Seconds> retry_after;
};

[[nodiscard]] bool is_success(const curl::Response& response) noexcept;

// No response at all. Anything that may have been the network is worth another attempt.
[[nodiscard]] Failed failed(const curl::Failure& failure) noexcept;
// A response outside 2xx; its error document, if any, decides.
[[nodiscard]] Failed failed(const curl::Response& response);
// A 2xx body that is not the expected document. An <Error> inside a 200 is a late failure; a
// body that does not parse is treated as cut short in transit.
[[nodiscard]] Failed failed(const s3util::ResponseError& error) noexcept;

} // namespace infra::storage::s3
