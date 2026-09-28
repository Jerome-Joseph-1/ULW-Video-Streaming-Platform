#pragma once

#include "core/util/time.hpp"
#include "infra/curl/http.hpp"
#include "infra/curl/multi.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace infra::curl {

// GETs small documents (a key set, a manifest) on the reactor. Reactor thread only.
class HttpFetcher {
public:
    // Runs once per fetch, on the reactor thread, never from inside get().
    using Callback = std::move_only_function<void(Result) noexcept>;

    // A fetch still unanswered after `timeout` fails with FailureKind::Timeout.
    HttpFetcher(Multi& multi, core::Millis timeout) noexcept;
    // Pending fetches are cancelled; their callbacks never run.
    ~HttpFetcher();
    HttpFetcher(const HttpFetcher&) = delete;
    HttpFetcher& operator=(const HttpFetcher&) = delete;

    // A 2xx body longer than `max_body` fails the fetch with BodyTooLarge rather than
    // arriving cut short.
    [[nodiscard]] std::expected<void, Failure> get(std::string url, std::size_t max_body,
                                                   Callback done);
    [[nodiscard]] std::size_t pending() const noexcept { return fetches_.size(); }

private:
    class Fetch;

    void finished(Fetch& fetch, Result result) noexcept;

    Multi& multi_;
    core::Millis timeout_;
    std::vector<std::unique_ptr<Fetch>> fetches_;
};

} // namespace infra::curl
