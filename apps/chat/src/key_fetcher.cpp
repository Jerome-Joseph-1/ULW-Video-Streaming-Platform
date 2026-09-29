#include "key_fetcher.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace chat {

namespace {

// A key set of ten RSA-4096 keys is ~8 KiB of JSON; anything past 64 KiB is not a key set.
constexpr std::size_t kMaxKeySet = std::size_t{64} * 1024;
constexpr int kHttpOk = 200;
// Requests that need a key the verifier has not seen wait on this fetch, and nothing but the
// six-hour request backstop bounds that wait otherwise. A healthy fetch is a 3 s connect at
// worst, a TLS handshake and a body under 64 KiB, well inside 10 s; a slower key server is
// treated as down, which answers the waiting clients 503 and lets them retry. 10 s is also
// the header timeout, the longest a client is otherwise kept waiting before its request starts.
constexpr core::Millis kFetchTimeout{10'000};

} // namespace

KeySetFetcher::KeySetFetcher(infra::curl::Multi& multi) noexcept : http_(multi, kFetchTimeout) {}

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

} // namespace chat
