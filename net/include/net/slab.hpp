#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace net {

// Fixed-capacity storage for per-connection objects, allocated once at startup. Handles carry
// a generation so a stale handle resolves to nothing instead of to the slot's next occupant.
// Destruction is deferred: retire() queues an object and reap() destroys it once the caller
// confirms nothing (the kernel included) can still reach it.
template <class T> class Slab {
public:
    struct Handle {
        std::uint32_t index = 0;
        std::uint32_t gen = 0;
        friend bool operator==(Handle, Handle) = default;
    };

    explicit Slab(std::size_t capacity) : slots_(capacity) {
        free_.reserve(capacity);
        retired_.reserve(capacity);
        for (std::size_t i = capacity; i > 0; --i) {
            free_.push_back(static_cast<std::uint32_t>(i - 1));
        }
    }

    // T is constructed as T(Handle, args...) so it can later retire itself.
    template <class... Args> [[nodiscard]] std::optional<Handle> emplace(Args&&... args) {
        if (free_.empty()) {
            return std::nullopt;
        }
        const std::uint32_t index = free_.back();
        free_.pop_back();
        Slot& s = slots_[index];
        const Handle h{.index = index, .gen = s.gen};
        s.value.emplace(h, std::forward<Args>(args)...);
        s.live = true;
        ++live_;
        return h;
    }

    [[nodiscard]] T* get(Handle h) noexcept {
        if (h.index >= slots_.size()) {
            return nullptr;
        }
        Slot& s = slots_[h.index];
        if (!s.live || s.retired || s.gen != h.gen || !s.value.has_value()) {
            return nullptr;
        }
        return std::addressof(s.value.value());
    }

    void retire(Handle h) noexcept {
        if (get(h) == nullptr) {
            return;
        }
        slots_[h.index].retired = true;
        retired_.push_back(h.index);
    }

    // Destroys every retired object for which `ready(T&)` holds. Returns how many remain.
    template <class Ready> std::size_t reap(Ready ready) noexcept {
        std::size_t kept = 0;
        for (const std::uint32_t index : retired_) {
            Slot& s = slots_[index];
            if (s.value.has_value() && !ready(*s.value)) {
                retired_[kept++] = index;
                continue;
            }
            s.value.reset();
            s.live = s.retired = false;
            ++s.gen;
            free_.push_back(index);
            --live_;
        }
        retired_.resize(kept);
        return kept;
    }

    template <class Fn> void for_each_live(Fn fn) {
        for (Slot& s : slots_) {
            if (s.live && !s.retired && s.value.has_value()) {
                fn(*s.value);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return live_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t gen = 1;
        bool live = false;
        bool retired = false;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::vector<std::uint32_t> retired_;
    std::size_t live_ = 0;
};

} // namespace net
