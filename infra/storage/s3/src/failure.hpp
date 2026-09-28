#pragma once

#include "core/ports/storage.hpp"
#include "core/util/time.hpp"
#include "infra/curl/http.hpp"
#include "infra/s3util/xml.hpp"

#include <atomic>
#include <cstdint>
#include <optional>

namespace infra::storage::s3 {

// Why one attempt at a request did not produce what was asked for.
struct Failed {
    core::ports::StorageError error = core::ports::StorageError::Permanent;
    // Honoured as a floor by the retry policy.
    std::optional<core::Seconds> retry_after;
    // Only a fix on our side can cure it; see s3util::MappedError.
    bool page = false;
};

// Failures that need a person to look (Failed::page). The client involved sees an ordinary
// error and there is no log to tell anyone else, so this count is where they show. Noted from
// the pool threads running control operations and from the reactor thread streaming parts.
class PageCount {
public:
    void note(const Failed& failure) noexcept {
        if (failure.page) {
            count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> count_{0};
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
