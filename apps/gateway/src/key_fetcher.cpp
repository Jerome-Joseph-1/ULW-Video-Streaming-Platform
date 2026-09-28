#include "key_fetcher.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace gateway {

namespace {

// A key set of ten RSA-4096 keys is ~8 KiB of JSON; anything past 64 KiB is not a key set.
constexpr std::size_t kMaxKeySet = std::size_t{64} * 1024;
constexpr int kHttpOk = 200;

} // namespace

void KeySetFetcher::fetch(std::string_view url, infra::auth::IKeySetReceiver& receiver) noexcept {
    const std::uint64_t id = next_fetch_++;
    waiting_.push_back({.receiver = &receiver, .fetch = id});
    auto started = http_.get(std::string(url), kMaxKeySet,
                             [this, id](auto result) noexcept { finished(id, std::move(result)); });
    if (!started) {
        // The verifier accepts a result delivered from inside fetch().
        finished(id, std::unexpected(std::move(started.error())));
    }
}

void KeySetFetcher::cancel(infra::auth::IKeySetReceiver& receiver) noexcept {
    std::erase_if(waiting_, [&](const Waiting& w) { return w.receiver == &receiver; });
}

void KeySetFetcher::finished(std::uint64_t fetch, infra::curl::Result result) noexcept {
    const auto it = std::ranges::find(waiting_, fetch, &Waiting::fetch);
    if (it == waiting_.end()) {
        return;
    }
    infra::auth::IKeySetReceiver& receiver = *it->receiver;
    waiting_.erase(it);
    if (!result || result->status != kHttpOk) {
        receiver.on_key_set(std::nullopt);
        return;
    }
    receiver.on_key_set(result->body);
}

} // namespace gateway
