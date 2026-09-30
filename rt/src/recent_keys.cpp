#include "recent_keys.hpp"

#include <functional>

namespace rt {

std::size_t RecentKeys::EntryHash::operator()(const Entry& e) const noexcept {
    // boost::hash_combine's mixing: the three hashes are independent, so any fair mix will do.
    constexpr std::size_t kGolden = 0x9e3779b97f4a7c15ULL;
    std::size_t h = std::hash<core::RoomId>{}(e.room);
    for (const std::size_t part :
         {std::hash<core::UserId>{}(e.sender), std::hash<MessageKey>{}(e.key)}) {
        h ^= part + kGolden + (h << 6U) + (h >> 2U);
    }
    return h;
}

std::uint64_t RecentKeys::digest(std::span<const std::byte> body) noexcept {
    // The 64-bit FNV offset basis and prime.
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const std::byte b : body) {
        h ^= std::to_integer<std::uint64_t>(b);
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::optional<RecentKeys::Sequenced> RecentKeys::find(const core::RoomId& room,
                                                      const core::UserId& sender,
                                                      const MessageKey& key) const {
    const auto it = seqs_.find(Entry{.room = room, .sender = sender, .key = key});
    if (it == seqs_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RecentKeys::remember(const core::RoomId& room, const core::UserId& sender,
                          const MessageKey& key, Sequenced sequenced, core::MonoTime now) {
    while (!order_.empty() && (order_.size() >= capacity_ || now - order_.front().at > window_)) {
        seqs_.erase(seqs_.find(*order_.front().entry));
        order_.pop_front();
    }
    const auto [it, fresh] =
        seqs_.try_emplace(Entry{.room = room, .sender = sender, .key = key}, sequenced);
    if (!fresh) {
        return;
    }
    try {
        order_.push_back({.entry = &it->first, .at = now});
    } catch (...) {
        // An entry nothing would ever age out is worse than one not remembered.
        seqs_.erase(it);
        throw;
    }
}

} // namespace rt
