#include "call.hpp"

#include <array>
#include <chrono>
#include <string_view>
#include <utility>

namespace chat {

namespace {

// The layouts' first byte; a node that reads another is answered Unavailable, as for an owner
// that cannot be reached.
constexpr std::uint8_t kLayout = 1;
constexpr std::size_t kDeviceBytes = core::Uuid::kTextLength;

void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void put_text(std::vector<std::byte>& out, std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text});
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// Endpoints and credentials are a URL and a JWT: well under 64 KiB.
void put_long(std::vector<std::byte>& out, std::string_view text) {
    put_u8(out, static_cast<std::uint8_t>((text.size() >> 8U) & 0xFFU));
    put_u8(out, static_cast<std::uint8_t>(text.size() & 0xFFU));
    put_text(out, text);
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : rest_(bytes) {}

    [[nodiscard]] std::optional<std::uint8_t> u8() noexcept {
        if (rest_.empty()) {
            return std::nullopt;
        }
        const auto v = std::to_integer<std::uint8_t>(rest_.front());
        rest_ = rest_.subspan(1);
        return v;
    }

    [[nodiscard]] std::optional<std::uint64_t> u64() noexcept {
        if (rest_.size() < sizeof(std::uint64_t)) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (const std::byte b : rest_.first(sizeof(std::uint64_t))) {
            v = (v << 8U) | std::to_integer<std::uint64_t>(b);
        }
        rest_ = rest_.subspan(sizeof(std::uint64_t));
        return v;
    }

    [[nodiscard]] std::optional<std::string_view> text(std::size_t n) noexcept {
        if (rest_.size() < n) {
            return std::nullopt;
        }
        // The bytes are characters; reading them as such is what this layout means.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::string_view out{reinterpret_cast<const char*>(rest_.data()), n};
        rest_ = rest_.subspan(n);
        return out;
    }

    [[nodiscard]] std::optional<std::string_view> short_text() noexcept {
        return u8().and_then([this](std::uint8_t n) { return text(n); });
    }

    [[nodiscard]] std::optional<std::string_view> long_text() noexcept {
        const auto high = u8();
        const auto low = u8();
        if (!high || !low) {
            return std::nullopt;
        }
        return text((std::size_t{*high} << 8U) | *low);
    }

    [[nodiscard]] bool empty() const noexcept { return rest_.empty(); }

private:
    std::span<const std::byte> rest_;
};

} // namespace

std::vector<std::byte> encode_request(const CallRequest& request) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, static_cast<std::uint8_t>(request.user.view().size()));
    put_text(out, request.user.view());
    std::array<char, kDeviceBytes> device{};
    request.device.format_to(device);
    put_text(out, {device.data(), device.size()});
    return out;
}

std::optional<CallRequest> decode_request(std::span<const std::byte> bytes) {
    Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto user = in.short_text().transform(core::UserId::parse);
    const auto device = in.text(kDeviceBytes).transform(core::DeviceId::parse);
    if (!user || !*user || !device || !*device || !in.empty()) {
        return std::nullopt;
    }
    return CallRequest{.user = **user, .device = **device};
}

std::vector<std::byte> encode_answer(const CallAnswer& answer) {
    std::vector<std::byte> out;
    put_u8(out, kLayout);
    put_u8(out, static_cast<std::uint8_t>(answer.outcome));
    if (answer.outcome == CallOutcome::Ticket && answer.ticket) {
        put_long(out, answer.ticket->endpoint);
        put_long(out, answer.ticket->credential);
        const auto ms =
            std::chrono::duration_cast<core::Millis>(answer.ticket->expires_at.time_since_epoch())
                .count();
        put_u64(out, static_cast<std::uint64_t>(ms));
    }
    return out;
}

std::optional<CallAnswer> decode_answer(std::span<const std::byte> bytes) {
    Reader in{bytes};
    if (in.u8() != kLayout) {
        return std::nullopt;
    }
    const auto outcome = in.u8();
    if (!outcome || *outcome > static_cast<std::uint8_t>(CallOutcome::Busy)) {
        return std::nullopt;
    }
    CallAnswer answer{.outcome = static_cast<CallOutcome>(*outcome), .ticket = std::nullopt};
    if (answer.outcome == CallOutcome::Ticket) {
        const auto endpoint = in.long_text();
        const auto credential = in.long_text();
        const auto ms = in.u64();
        if (!endpoint || !credential || !ms) {
            return std::nullopt;
        }
        answer.ticket = core::ports::MediaTicket{
            .endpoint = std::string(*endpoint),
            .credential = std::string(*credential),
            .expires_at = core::WallTime{core::Millis{static_cast<core::Millis::rep>(*ms)}}};
    }
    if (!in.empty()) {
        return std::nullopt;
    }
    return answer;
}

CallHandler::CallHandler(core::ports::IMessageStore& messages, core::ports::ISfu* sfu,
                         const core::ports::IClock& clock, CallLimits limits)
    : messages_(messages), sfu_(sfu), clock_(clock), limits_(limits), next_sweep_(clock.now()) {}

CallHandler::~CallHandler() = default;

void CallHandler::on_ask(const core::RoomId& room, std::span<const std::byte> request,
                         rt::OwnerAnswer answer) noexcept {
    bool counted = false;
    try {
        const auto asked = decode_request(request);
        if (!asked) {
            answer(std::unexpected(rt::RouteError::Unavailable));
            return;
        }
        if (sfu_ == nullptr) {
            finish(answer, {.outcome = CallOutcome::Disabled, .ticket = std::nullopt});
            return;
        }
        if (in_flight_ >= limits_.max_in_flight) {
            ++counters_.busy;
            finish(answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
            return;
        }
        ++in_flight_;
        counted = true;
        // The member list, read on the owner at the moment of asking: a client's join may be
        // older than a removal still on its way to its node (ADR-0073).
        messages_.access(
            room, asked->user,
            [this, room, waiter = Waiter{.request = *asked, .answer = std::move(answer)}](
                core::ports::MessageResult<core::ports::RoomAccess> access) mutable noexcept {
                checked(room, std::move(waiter), access);
            });
    } catch (const std::bad_alloc&) {
        if (counted) {
            --in_flight_;
        }
        // Whatever was moved out of `answer` answers nothing; the asker times out.
        if (answer) {
            answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::checked(const core::RoomId& room, Waiter waiter,
                          core::ports::MessageResult<core::ports::RoomAccess> access) noexcept {
    if (!access) {
        ++counters_.store_unavailable;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::Unavailable, .ticket = std::nullopt});
        return;
    }
    if (access->kind != core::ports::RoomKind::DirectChat) {
        ++counters_.not_callable;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::NotCallable, .ticket = std::nullopt});
        return;
    }
    if (!access->member) {
        ++counters_.not_member;
        --in_flight_;
        finish(waiter.answer, {.outcome = CallOutcome::NotMember, .ticket = std::nullopt});
        return;
    }
    admit(room, std::move(waiter));
}

void CallHandler::admit(const core::RoomId& room, Waiter waiter) noexcept {
    try {
        auto it = rooms_.find(room);
        if (it == rooms_.end()) {
            if (rooms_.size() >= limits_.max_rooms) {
                sweep_now();
            }
            if (rooms_.size() >= limits_.max_rooms) {
                ++counters_.busy;
                --in_flight_;
                finish(waiter.answer, {.outcome = CallOutcome::Busy, .ticket = std::nullopt});
                return;
            }
            it = rooms_.try_emplace(room).first;
        }
        Entry& entry = it->second;
        entry.used = clock_.now();
        if (entry.media) {
            join(room, entry, std::move(waiter));
            return;
        }
        // Opening already: this ask waits for it, and the room is opened once however many
        // asked meanwhile.
        const bool first = entry.opening.empty();
        entry.opening.push_back(std::move(waiter));
        if (!first) {
            return;
        }
        try {
            sfu_->open_room(
                room, kCallGeneration, core::ports::MediaRoomKind::Call, kCallParticipants,
                [this, room](
                    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
                        result) noexcept { opened(room, std::move(result)); });
        } catch (const std::bad_alloc&) {
            // Nothing will answer the waiters, this one or any that would join them later.
            abandon_open(room);
        }
    } catch (const std::bad_alloc&) {
        // Before the waiter was queued: it is still this call's to answer.
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::abandon_open(const core::RoomId& room) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    std::vector<Waiter> waiting = std::exchange(it->second.opening, {});
    if (!it->second.media && it->second.joining == 0) {
        rooms_.erase(it);
    }
    for (Waiter& w : waiting) {
        --in_flight_;
        w.answer(std::unexpected(rt::RouteError::Unavailable));
    }
}

void CallHandler::opened(
    const core::RoomId& room,
    std::expected<std::unique_ptr<core::ports::IMediaRoom>, core::ports::MediaError>
        result) noexcept {
    const auto it = rooms_.find(room);
    if (it == rooms_.end()) {
        return;
    }
    Entry& entry = it->second;
    std::vector<Waiter> waiting = std::exchange(entry.opening, {});
    if (!result) {
        const CallOutcome outcome = failure(result.error());
        for (Waiter& w : waiting) {
            --in_flight_;
            finish(w.answer, {.outcome = outcome, .ticket = std::nullopt});
        }
        if (!entry.media && entry.joining == 0) {
            rooms_.erase(it);
        }
        return;
    }
    ++counters_.opens;
    entry.media = std::move(*result);
    for (Waiter& w : waiting) {
        join(room, entry, std::move(w));
    }
}

void CallHandler::join(const core::RoomId& room, Entry& entry, Waiter waiter) noexcept {
    try {
        ++entry.joining;
        const core::UserId user = waiter.request.user;
        const core::DeviceId device = waiter.request.device;
        entry.media->join(
            user, device, core::ports::MediaRole::Member,
            [this, room, answer = std::move(waiter.answer)](
                std::expected<core::ports::MediaTicket, core::ports::MediaError>
                    ticket) mutable noexcept {
                --in_flight_;
                if (const auto it = rooms_.find(room); it != rooms_.end()) {
                    --it->second.joining;
                }
                if (!ticket) {
                    finish(answer, {.outcome = failure(ticket.error()), .ticket = std::nullopt});
                    return;
                }
                ++counters_.tickets;
                finish(answer, {.outcome = CallOutcome::Ticket, .ticket = std::move(*ticket)});
            });
    } catch (const std::bad_alloc&) {
        --entry.joining;
        --in_flight_;
        if (waiter.answer) {
            waiter.answer(std::unexpected(rt::RouteError::Unavailable));
        }
    }
}

void CallHandler::finish(rt::OwnerAnswer& answer, const CallAnswer& outcome) noexcept {
    try {
        answer(encode_answer(outcome));
    } catch (const std::bad_alloc&) {
        answer(std::unexpected(rt::RouteError::Unavailable));
    }
}

CallOutcome CallHandler::failure(core::ports::MediaError error) noexcept {
    switch (error) {
    case core::ports::MediaError::Unavailable:
        ++counters_.sfu_unavailable;
        return CallOutcome::Unavailable;
    case core::ports::MediaError::Refused:
    case core::ports::MediaError::Closed:
    case core::ports::MediaError::NotImplemented:
        ++counters_.sfu_refused;
        return CallOutcome::Failed;
    }
    ++counters_.sfu_refused;
    return CallOutcome::Failed;
}

void CallHandler::sweep() noexcept {
    // Called after every turn of the loop; the rooms are looked at once a second.
    constexpr core::Millis kSweepEvery{1'000};
    const core::MonoTime now = clock_.now();
    if (now < next_sweep_) {
        return;
    }
    next_sweep_ = now + kSweepEvery;
    sweep_now();
}

void CallHandler::sweep_now() noexcept {
    const core::MonoTime now = clock_.now();
    std::erase_if(rooms_, [&](const auto& entry) {
        const Entry& e = entry.second;
        return e.opening.empty() && e.joining == 0 && now - e.used >= limits_.idle;
    });
}

} // namespace chat
