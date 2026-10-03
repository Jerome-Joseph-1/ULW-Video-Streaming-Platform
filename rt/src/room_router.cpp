#include "rt/room_router.hpp"

#include "net/slab.hpp"
#include "net/socket.hpp"

#include "node_auth.hpp"
#include "recent_keys.hpp"
#include "wire.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace rt {

namespace {

// Forward deadlines are checked this often. The reactor's timer wheel ticks every 100 ms, and
// a deadline a quarter second late costs nothing next to the timeout itself.
constexpr core::Millis kTick{250};
// An owner answers a forward after one append, which the store gives up on after
// kStoreTimeout; the extra second covers the hop and the writes queued ahead in the room.
constexpr core::Millis kForwardTimeout = kStoreTimeout + core::Millis{1'000};
// Writes waiting for their sequence number are counted in bytes: the body, plus this for the
// rest of a Write (sender, callback, queue slot), so that empty bodies are not free.
constexpr std::size_t kWriteOverhead = 256;
// One room sequences about one message per store round trip, a millisecond or so. 1 MiB is a
// few hundred ordinary messages, a quarter second or more of backlog, or sixteen of the
// largest; past that the room is flooded, and the owner sheds load with Busy.
constexpr std::size_t kMaxRoomQueueBytes = std::size_t{1} << 20U;
// All rooms' queues together: 64 of them full. The owner's share of the node's memory budget
// (ADR-0036), whatever mix of rooms is busy.
constexpr std::size_t kMaxQueueBytes = std::size_t{64} << 20U;
// Frames waiting for a connection to another node to open: a handful of subscriptions and
// forwards. A peer that takes longer to answer than this fills is treated as down.
constexpr std::size_t kMaxUnsentBytes = std::size_t{1} << 20U;
// Connections from other nodes at once. A deployment runs three replicas, each dialling this
// node once; the rest is room for the dead ones a rolling restart leaves until they close, and
// for scaling out. Each may hold a frame in its decoder and kMaxPeerBacklog unsent.
constexpr std::size_t kMaxPeers = 32;
// Connections still in their handshake, which anyone who reaches the port can open. They get
// slots of their own, and past this many the oldest is dropped for the newest: a flood of
// idle connections churns among themselves instead of filling the slots real nodes need, and
// a real node's handshake, done in milliseconds, is never the oldest for long.
constexpr std::size_t kMaxHandshaking = 8;
// Slots beyond both, for connections closed and not yet reaped.
constexpr std::size_t kInboundSlots = kMaxPeers + (2 * kMaxHandshaking);
// From a connection's start to the end of its handshake: a round trip on the private network
// and two MACs take milliseconds, and the dialer's address lookup is bounded by kStoreTimeout.
// A peer that takes longer is stuck or is not a node, and gives its slot back.
constexpr core::Millis kHandshakeTimeout{5'000};
// Unsent bytes a node-channel connection may hold: sixteen of the largest deliveries. A peer
// that far behind has stopped reading (paused, or wedged), and every message of every room it
// subscribed to would otherwise queue here without end; the connection is closed, and the
// peer resubscribes when it reconnects.
constexpr std::size_t kMaxPeerBacklog = std::size_t{16} * wire::kMaxFrame;
// A client sends again after `unavailable`, which comes within kForwardTimeout, plus its own
// backoff, or after reconnecting: seconds. A minute of keys covers both with room to spare.
constexpr core::Millis kRecentKeysWindow{60'000};
// Each key remembered costs about 300 bytes (room, sender, key and seq, and the map's and the
// queue's own), so 32768 are about 10 MiB of the node's budget. That is a minute of 546
// messages a second sequenced or delivered here, all rooms and senders together; past that the
// window shrinks for everyone, to half a minute at the thousand a second one room can reach
// (ADR-0035): still ten forward timeouts.
constexpr std::size_t kRecentKeys = 32'768;
// Rooms whose owner a notice is waiting to learn, each one read of the store's owners, and
// notices each may hold meanwhile. A notice is a few hundred bytes (a call's ring, ADR-0092):
// 256 rooms of 8 hold at most 8 MiB at kMaxOwnerMessage, far less as sent. Past either, a
// notice is dropped, as one whose owner cannot be reached is.
constexpr std::size_t kMaxNoticeLookups = 256;
constexpr std::size_t kMaxNoticesPerLookup = 8;

// A Reply carries a seq, an Answer a body; each leaves the other empty.
using RequestDone = std::move_only_function<void(wire::Status, std::uint64_t seq,
                                                 std::span<const std::byte> body) noexcept>;

// Whether a node-channel connection's output is getting through to the other node (ADR-0071).
// It stands in for the kernel's user timeout, which Linux counts from the first probe of a shut
// window and restarts only when the window opens wide enough for the whole unsent head of the
// queue: it ended a busy node that kept reading, a little at a time, as if it had vanished.
// Here any acknowledgement counts, however little it lets through.
class StallWatch {
public:
    // Clears the kernel's user timeout where TCP_INFO can stand in for it. Where it cannot,
    // the connection keeps the timeout and is never found stalled here.
    void take_over(int fd) noexcept {
        active_ = net::send_progress(fd).has_value() && net::clear_user_timeout(fd).has_value();
    }

    // True once output has waited `limit` for the other node with none of it acknowledged.
    // `queued` is what the reactor still holds for the connection; the kernel says the rest.
    [[nodiscard]] bool stalled(int fd, std::size_t queued, core::MonoTime now,
                               core::Millis limit) noexcept {
        if (!active_) {
            return false;
        }
        const auto progress = net::send_progress(fd);
        if (!progress) {
            return false;
        }
        if (queued == 0 && !progress->waiting) {
            watching_ = false;
            return false;
        }
        if (!watching_ || progress->acked != acked_) {
            watching_ = true;
            acked_ = progress->acked;
            since_ = now;
            return false;
        }
        return now - since_ >= limit;
    }

private:
    core::MonoTime since_;
    std::uint64_t acked_ = 0;
    bool active_ = false;
    bool watching_ = false;
};

RouteError route_error(AppendError error) noexcept {
    switch (error) {
    case AppendError::Fenced:
        return RouteError::Fenced;
    case AppendError::Conflict:
        return RouteError::Conflict;
    case AppendError::Unavailable:
        return RouteError::Unavailable;
    }
    return RouteError::Unavailable;
}

RouteError route_error(wire::Status status) noexcept {
    switch (status) {
    case wire::Status::Fenced:
        return RouteError::Fenced;
    case wire::Status::Busy:
        return RouteError::Busy;
    case wire::Status::Conflict:
        return RouteError::Conflict;
    case wire::Status::Ok:
    case wire::Status::NotOwner:
    case wire::Status::Unavailable:
        return RouteError::Unavailable;
    }
    return RouteError::Unavailable;
}

wire::Status wire_status(RouteError error) noexcept {
    switch (error) {
    case RouteError::Fenced:
        return wire::Status::Fenced;
    case RouteError::Busy:
        return wire::Status::Busy;
    case RouteError::Conflict:
        return wire::Status::Conflict;
    case RouteError::NotJoined:
    case RouteError::Unavailable:
        return wire::Status::Unavailable;
    }
    return wire::Status::Unavailable;
}

} // namespace

class RoomRouter::Impl final : public IRegistryObserver,
                               public net::IAcceptHandler,
                               public net::ITimerHandler {
    // A connection another node opened to this one, to subscribe and to forward writes.
    class Inbound final : public net::IStreamHandler {
    public:
        using Handle = net::Slab<Inbound>::Handle;

        Inbound(Handle handle, Impl& router) noexcept : handle_(handle), router_(router) {}
        Inbound(const Inbound&) = delete;
        Inbound& operator=(const Inbound&) = delete;
        Inbound(Inbound&&) = delete;
        Inbound& operator=(Inbound&&) = delete;
        ~Inbound() override = default;

        void start(net::ConnId conn) noexcept {
            conn_ = conn;
            accepted_ = router_.clock_.now();
            stall_.take_over(conn_.fd);
            router_.reactor_.start_receiving(conn_);
        }

        [[nodiscard]] net::ConnId conn() const noexcept { return conn_; }

        // Frames for the dialer. Only an authenticated peer is ever sent any.
        void send(std::span<const std::byte> frame) noexcept {
            if (closed_ || state_ != State::Authenticated) {
                return;
            }
            router_.reactor_.send(conn_, frame);
            if (router_.reactor_.pending_send_bytes(conn_) > kMaxPeerBacklog) {
                cut_off();
            }
        }

        [[nodiscard]] bool authenticated() const noexcept { return state_ == State::Authenticated; }

        // Output has waited `limit` for the dialer, which acknowledged none of it.
        [[nodiscard]] bool stalled(core::MonoTime now, core::Millis limit) noexcept {
            return !closed_ && state_ == State::Authenticated &&
                   stall_.stalled(conn_.fd, router_.reactor_.pending_send_bytes(conn_), now, limit);
        }

        // Closes a dialer that stopped reading, with a reset: the kernel drops what it still
        // holds for it at once, instead of keeping it for a peer that no longer reads.
        void cut_off() noexcept {
            if (closed_) {
                return;
            }
            ++router_.counters_.slow_peers;
            net::abort_on_close(conn_.fd);
            close();
        }

        [[nodiscard]] core::MonoTime accepted() const noexcept { return accepted_; }

        [[nodiscard]] bool handshake_overdue(core::MonoTime now) const noexcept {
            return state_ != State::Authenticated && now - accepted_ > kHandshakeTimeout;
        }

        void on_data(net::BorrowedBytes bytes) noexcept override {
            try {
                decoder_.feed(bytes);
                while (!closed_) {
                    auto next = decoder_.next();
                    if (!next) {
                        close();
                        return;
                    }
                    const std::optional<wire::Frame>& frame = *next;
                    if (!frame) {
                        return;
                    }
                    if (!handle(*frame)) {
                        close();
                        return;
                    }
                }
            } catch (const std::bad_alloc&) {
                ++router_.counters_.allocation_failures;
                close();
            }
        }

        void on_writable() noexcept override {}
        void on_peer_eof() noexcept override { close(); }
        void on_error(int /*err*/) noexcept override { close(); }

        void close() noexcept {
            if (closed_) {
                return;
            }
            closed_ = true;
            router_.reactor_.begin_close(conn_);
            router_.inbound_.retire(handle_);
        }

    private:
        enum class State : std::uint8_t { AwaitHello, AwaitProof, Authenticated };

        // false: the peer broke the protocol, or is not a node holding the secret.
        bool handle(const wire::Frame& frame) {
            switch (state_) {
            case State::AwaitHello:
                return greet(std::get_if<wire::Hello>(&frame));
            case State::AwaitProof:
                return verify(std::get_if<wire::Proof>(&frame));
            case State::Authenticated:
                return serve(frame);
            }
            return false;
        }

        bool greet(const wire::Hello* hello) {
            if (hello == nullptr) {
                router_.refused("no hello");
                return false;
            }
            // Nodes on two versions of the channel refuse each other: say so, or a rollout
            // across a version reads like a stranger knocking.
            if (hello->version != wire::kVersion) {
                router_.refused(std::format("version mismatch: peer speaks {}, this node {}",
                                            hello->version, wire::kVersion));
                return false;
            }
            dialer_.emplace(hello->node);
            dialer_nonce_ = hello->nonce;
            router_.random_.fill(own_nonce_);
            const auto tag = auth::acceptor_tag(router_.secret(), hello->node, router_.config_.self,
                                                dialer_nonce_, own_nonce_);
            if (!tag) {
                return false;
            }
            std::vector<std::byte> challenge;
            wire::encode_challenge(challenge, router_.config_.self, own_nonce_, *tag);
            router_.reactor_.send(conn_, challenge);
            state_ = State::AwaitProof;
            return true;
        }

        bool verify(const wire::Proof* proof) {
            if (proof == nullptr || !dialer_) {
                router_.refused("no proof");
                return false;
            }
            const auto expected = auth::dialer_tag(router_.secret(), *dialer_, router_.config_.self,
                                                   dialer_nonce_, own_nonce_);
            if (!expected || !auth::same_tag(*expected, proof->mac)) {
                router_.refused("bad proof");
                return false;
            }
            if (router_.authenticated_peers() >= kMaxPeers) {
                router_.refused("too many peers");
                return false;
            }
            state_ = State::Authenticated;
            return true;
        }

        bool serve(const wire::Frame& frame) {
            if (const auto* f = std::get_if<wire::Subscribe>(&frame)) {
                router_.on_subscribe(handle_, f->request, f->room);
                return true;
            }
            if (const auto* f = std::get_if<wire::Unsubscribe>(&frame)) {
                router_.on_unsubscribe(handle_, f->room);
                return true;
            }
            if (const auto* f = std::get_if<wire::Send>(&frame)) {
                router_.on_forwarded(handle_, f->request, f->room, f->sender, f->key, f->body);
                return true;
            }
            if (const auto* f = std::get_if<wire::Ask>(&frame)) {
                router_.on_ask(handle_, f->request, f->room, f->body);
                return true;
            }
            if (const auto* f = std::get_if<wire::Notify>(&frame)) {
                router_.on_notify(f->room, f->body);
                return true;
            }
            return false;
        }

        Handle handle_;
        Impl& router_;
        net::ConnId conn_;
        core::MonoTime accepted_;
        wire::Decoder decoder_;
        State state_ = State::AwaitHello;
        std::optional<core::NodeId> dialer_;
        wire::Nonce dialer_nonce_{};
        wire::Nonce own_nonce_{};
        StallWatch stall_;
        bool closed_ = false;
    };

    // This node's connection to another, dialled on first use: subscriptions and forwarded writes
    // go out, replies and deliveries come back.
    class Outbound final : public net::IStreamHandler, public net::IReadyHandler {
    public:
        Outbound(Impl& router, core::NodeId node, std::uint64_t id) noexcept
            : router_(router), node_(node), id_(id), created_(router.clock_.now()) {}
        Outbound(const Outbound&) = delete;
        Outbound& operator=(const Outbound&) = delete;
        Outbound(Outbound&&) = delete;
        Outbound& operator=(Outbound&&) = delete;
        ~Outbound() override { close(); }

        [[nodiscard]] const core::NodeId& node() const noexcept { return node_; }

        void locate() {
            router_.store_.find_address(
                node_, [&router = router_, node = node_,
                        id = id_](StoreResult<std::optional<std::string>> address) noexcept {
                    // The link may have failed and been replaced while the lookup ran.
                    const auto it = router.outbound_.find(node);
                    if (it == router.outbound_.end() || it->second->id_ != id) {
                        return;
                    }
                    it->second->connect(address);
                });
        }

        void request(std::uint64_t request, std::span<const std::byte> frame, RequestDone done,
                     core::Millis timeout = kForwardTimeout) {
            pending_.push_back({.request = request,
                                .deadline = router_.clock_.now() + timeout,
                                .done = std::move(done)});
            send(frame);
        }

        // Until the owner has proven itself, frames wait here; they reach nobody else.
        // A send that finds the link broken only marks it: send() runs inside the router's
        // own walks over its rooms, which taking the link down (failing its requests, whose
        // callbacks change those rooms) must not run under. The next tick takes it down.
        void send(std::span<const std::byte> frame) {
            if (broken_ || state_ == State::Closed) {
                return;
            }
            if (state_ == State::Open) {
                router_.reactor_.send(conn_, frame);
                if (router_.reactor_.pending_send_bytes(conn_) > kMaxPeerBacklog) {
                    cut_off();
                }
                return;
            }
            if (unsent_.size() + frame.size() > kMaxUnsentBytes) {
                broken_ = true;
                return;
            }
            unsent_.insert(unsent_.end(), frame.begin(), frame.end());
        }

        // Takes out the requests past their deadline, for the caller to fail once it has
        // stopped walking the links: a failure may open a new one.
        void expire(core::MonoTime now, std::vector<RequestDone>& expired) {
            std::erase_if(pending_, [&](Pending& p) {
                if (p.deadline > now) {
                    return false;
                }
                expired.push_back(std::move(p.done));
                return true;
            });
        }

        void fail_requests() noexcept {
            std::vector<Pending> failed = std::move(pending_);
            pending_.clear();
            for (Pending& p : failed) {
                p.done(wire::Status::Unavailable, 0, {});
            }
        }

        void close() noexcept {
            if (state_ == State::Connecting) {
                router_.reactor_.unwatch(connecting_.get());
                connecting_.reset();
            }
            if (state_ == State::Handshaking || state_ == State::Open) {
                if (reset_) {
                    net::abort_on_close(conn_.fd);
                }
                router_.reactor_.begin_close(conn_);
            }
            state_ = State::Closed;
        }

        // Output has waited `limit` for the owner, which acknowledged none of it.
        [[nodiscard]] bool stalled(core::MonoTime now, core::Millis limit) noexcept {
            return state_ == State::Open && !broken_ &&
                   stall_.stalled(conn_.fd, router_.reactor_.pending_send_bytes(conn_), now, limit);
        }

        // Marks an owner that stopped reading: the tick takes the link down, and closes it
        // with a reset, which drops what the kernel still holds for it at once.
        void cut_off() noexcept {
            ++router_.counters_.slow_peers;
            broken_ = true;
            reset_ = true;
        }

        // Broken by a send, or still not open past the handshake timeout.
        [[nodiscard]] bool due_down(core::MonoTime now) const noexcept {
            return broken_ || (state_ != State::Open && now - created_ > kHandshakeTimeout);
        }

        [[nodiscard]] bool quiescent(const net::IReactor& reactor) const noexcept {
            return !opened_ || reactor.is_quiescent(conn_);
        }

        // ---- connecting

        void on_ready(net::Interest /*ready*/) noexcept override {
            router_.reactor_.unwatch(connecting_.get());
            if (net::connect_result(connecting_.get()) != 0) {
                connecting_.reset();
                state_ = State::Closed;
                router_.link_down(*this);
                return;
            }
            [[maybe_unused]] const auto tuned = net::tune_connection(connecting_.get());
            stall_.take_over(connecting_.get());
            auto id = router_.reactor_.attach(std::move(connecting_), *this);
            if (!id) {
                state_ = State::Closed;
                router_.link_down(*this);
                return;
            }
            conn_ = *id;
            opened_ = true;
            state_ = State::Handshaking;
            router_.random_.fill(own_nonce_);
            std::vector<std::byte> hello;
            wire::encode_hello(hello, router_.config_.self, own_nonce_);
            router_.reactor_.start_receiving(conn_);
            router_.reactor_.send(conn_, hello);
        }

        // ---- open

        void on_data(net::BorrowedBytes bytes) noexcept override {
            try {
                decoder_.feed(bytes);
                while (state_ == State::Handshaking || state_ == State::Open) {
                    auto next = decoder_.next();
                    if (!next) {
                        router_.link_down(*this);
                        return;
                    }
                    const std::optional<wire::Frame>& frame = *next;
                    if (!frame) {
                        return;
                    }
                    if (!handle(*frame)) {
                        router_.link_down(*this);
                        return;
                    }
                }
            } catch (const std::bad_alloc&) {
                ++router_.counters_.allocation_failures;
                router_.link_down(*this);
            }
        }

        void on_writable() noexcept override {}
        void on_peer_eof() noexcept override { router_.link_down(*this); }
        void on_error(int /*err*/) noexcept override { router_.link_down(*this); }

    private:
        enum class State : std::uint8_t { Locating, Connecting, Handshaking, Open, Closed };

        struct Pending {
            std::uint64_t request;
            core::MonoTime deadline;
            RequestDone done;
        };

        void connect(const StoreResult<std::optional<std::string>>& address) {
            if (state_ != State::Locating) {
                return;
            }
            if (!address) {
                router_.link_down(*this);
                return;
            }
            const std::optional<std::string>& where = *address;
            if (!where) {
                router_.link_down(*this);
                return;
            }
            auto fd = net::start_connect(*where);
            if (!fd) {
                router_.link_down(*this);
                return;
            }
            connecting_ = std::move(*fd);
            if (!router_.reactor_.watch(connecting_.get(), net::Interest::Write, *this)) {
                connecting_.reset();
                router_.link_down(*this);
                return;
            }
            state_ = State::Connecting;
        }

        // false: the peer broke the protocol, or is not the node it should be.
        bool handle(const wire::Frame& frame) {
            if (state_ == State::Handshaking) {
                return answer(std::get_if<wire::Challenge>(&frame));
            }
            if (const auto* f = std::get_if<wire::Reply>(&frame)) {
                const auto it = std::ranges::find(pending_, f->request, &Pending::request);
                // Already failed by its deadline.
                if (it == pending_.end()) {
                    return true;
                }
                RequestDone done = std::move(it->done);
                pending_.erase(it);
                done(f->status, f->seq, {});
                return true;
            }
            if (const auto* f = std::get_if<wire::Answer>(&frame)) {
                const auto it = std::ranges::find(pending_, f->request, &Pending::request);
                if (it == pending_.end()) {
                    return true;
                }
                RequestDone done = std::move(it->done);
                pending_.erase(it);
                done(f->status, 0, f->body);
                return true;
            }
            if (const auto* f = std::get_if<wire::Deliver>(&frame)) {
                router_.on_deliver(Message{.room = f->room,
                                           .seq = f->seq,
                                           .sender = f->sender,
                                           .key = f->key,
                                           .body = f->body});
                return true;
            }
            if (const auto* f = std::get_if<wire::Unsubscribe>(&frame)) {
                router_.disowned(f->room, node_);
                return true;
            }
            if (const auto* f = std::get_if<wire::Notice>(&frame)) {
                router_.hear_here(f->room, f->body);
                return true;
            }
            return false;
        }

        // The owner proves itself first; only then does this node prove itself and send.
        bool answer(const wire::Challenge* challenge) {
            if (challenge == nullptr || challenge->node != node_) {
                router_.refused("no challenge");
                return false;
            }
            const auto expected = auth::acceptor_tag(router_.secret(), router_.config_.self, node_,
                                                     own_nonce_, challenge->nonce);
            if (!expected || !auth::same_tag(*expected, challenge->mac)) {
                router_.refused("bad challenge");
                return false;
            }
            const auto tag = auth::dialer_tag(router_.secret(), router_.config_.self, node_,
                                              own_nonce_, challenge->nonce);
            if (!tag) {
                return false;
            }
            std::vector<std::byte> proof;
            wire::encode_proof(proof, *tag);
            router_.reactor_.send(conn_, proof);
            router_.reactor_.send(conn_, unsent_);
            unsent_.clear();
            unsent_.shrink_to_fit();
            state_ = State::Open;
            return true;
        }

        Impl& router_;
        core::NodeId node_;
        std::uint64_t id_;
        core::MonoTime created_;
        bool broken_ = false;
        // Closed with a reset: the owner stopped reading.
        bool reset_ = false;
        wire::Nonce own_nonce_{};
        State state_ = State::Locating;
        os::UniqueFd connecting_;
        net::ConnId conn_;
        bool opened_ = false;
        std::vector<std::byte> unsent_;
        std::vector<Pending> pending_;
        wire::Decoder decoder_;
        StallWatch stall_;
    };

public:
    Impl(net::IReactor& reactor, IRoomStore& store, const core::ports::IClock& clock,
         core::ports::IRandom& random, RouterConfig config, IRouterEvents& events)
        : reactor_(reactor), store_(store), clock_(clock), random_(random),
          config_(std::move(config)), events_(events), incarnation_(core::Uuid::v7(clock, random)),
          registry_(store, clock, config_.self, incarnation_, *this),
          recent_(kRecentKeys, kRecentKeysWindow), inbound_(kInboundSlots) {}

    ~Impl() override {
        reactor_.cancel_timer(timer_);
        inbound_.for_each_live([](Inbound& in) { in.close(); });
        for (auto& [node, link] : outbound_) {
            link->close();
        }
        // Connections may still be in the kernel's hands; nothing calls back into them once
        // begin_close has run, so they go regardless.
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::expected<void, int> start(os::UniqueFd listener) {
        if (auto r = reactor_.listen(std::move(listener), *this); !r) {
            return r;
        }
        timer_ = reactor_.arm_timer(core::Millis{0}, *this);
        advertise();
        return {};
    }

    // ---- members on this node

    void join(const core::RoomId& room, IMember& member, JoinCallback done) {
        if (draining_) {
            done(std::unexpected(RouteError::Unavailable));
            return;
        }
        if (!local_.contains(room) && local_.size() >= config_.max_rooms) {
            done(std::unexpected(RouteError::Busy));
            return;
        }
        auto [it, created] = local_.try_emplace(room);
        LocalRoom& lr = it->second;
        if (created) {
            registry_.set_interest(room, true);
        }
        if (std::ranges::find(lr.members, &member) != lr.members.end()) {
            done(head(room));
            return;
        }
        if (lr.owner) {
            lr.members.push_back(&member);
            done(head(room));
            return;
        }
        lr.joining.push_back({.member = &member, .done = std::move(done)});
        find_owner(room);
    }

    void leave(const core::RoomId& room, IMember& member) noexcept {
        const auto it = local_.find(room);
        if (it == local_.end()) {
            return;
        }
        LocalRoom& lr = it->second;
        std::erase(lr.members, &member);
        std::erase_if(lr.joining, [&](const Joining& j) { return j.member == &member; });
        std::erase_if(sends_, [&](const auto& entry) {
            return entry.second.room == room && entry.second.member == &member;
        });
        std::erase_if(asks_, [&](const auto& entry) {
            return entry.second.room == room && entry.second.member == &member;
        });
        drop_if_empty(room);
    }

    void send(const core::RoomId& room, IMember& from, const core::UserId& sender,
              const MessageKey& key, std::vector<std::byte> body, SendCallback done) {
        const auto it = local_.find(room);
        if (it == local_.end() ||
            std::ranges::find(it->second.members, &from) == it->second.members.end()) {
            done(std::unexpected(RouteError::NotJoined));
            return;
        }
        // Delivered here already: whoever owns the room now, it was sequenced. Under another
        // body the key is spent, and nothing is sent.
        if (const auto seen = recent_.find(room, sender, key)) {
            if (seen->digest != RecentKeys::digest(body)) {
                done(std::unexpected(RouteError::Conflict));
                return;
            }
            ++counters_.duplicates;
            done(seen->seq);
            return;
        }
        // Owning comes first, whatever the cache says: a write under a generation that has
        // moved on is exactly what the fence must see and refuse.
        if (registry_.owned(room)) {
            enqueue(room, Write{.sender = sender,
                                .key = key,
                                .body = std::move(body),
                                .local = pending(room, from, std::move(done)),
                                .peer = std::nullopt,
                                .request = 0});
            return;
        }
        // The newest owner known, which a notification may have named before this node's
        // subscription has moved there; failing that, the one it subscribed to.
        const std::optional<Ownership> owner =
            registry_.known_owner(room).or_else([&] { return it->second.owner; });
        if (!owner || owner->node == config_.self) {
            done(std::unexpected(RouteError::Unavailable));
            return;
        }
        std::vector<std::byte> frame;
        const std::uint64_t request = next_request_++;
        wire::encode_send(frame, request, room, sender, key, body);
        ++counters_.forwarded;
        link(owner->node)
            .request(request, frame,
                     [this, room, done = pending(room, from, std::move(done))](
                         wire::Status status, std::uint64_t seq,
                         std::span<const std::byte> /*body*/) mutable noexcept {
                         if (status == wire::Status::Ok) {
                             done(seq);
                             return;
                         }
                         if (status == wire::Status::NotOwner) {
                             lost_owner(room);
                         }
                         done(std::unexpected(route_error(status)));
                     });
    }

    // Holds `done` for a send by `member`, and answers through it unless the member has left
    // the room by then: a member that left, or went away with its connection, is owed nothing,
    // and may no longer exist to be told.
    SendCallback pending(const core::RoomId& room, IMember& member, SendCallback done) {
        const std::uint64_t id = next_send_++;
        sends_.emplace(id, PendingSend{.room = room, .member = &member, .done = std::move(done)});
        return [this, id](std::expected<std::uint64_t, RouteError> result) noexcept {
            const auto it = sends_.find(id);
            if (it == sends_.end()) {
                return;
            }
            SendCallback answer = std::move(it->second.done);
            sends_.erase(it);
            answer(result);
        };
    }

    void ask_owner(const core::RoomId& room, IMember& from, std::span<const std::byte> request,
                   OwnerAnswer done) {
        const auto it = local_.find(room);
        if (it == local_.end() ||
            std::ranges::find(it->second.members, &from) == it->second.members.end()) {
            done(std::unexpected(RouteError::NotJoined));
            return;
        }
        if (request.size() > kMaxOwnerMessage) {
            done(std::unexpected(RouteError::Unavailable));
            return;
        }
        if (registry_.owned(room)) {
            answer_here(room, request, asking(room, from, std::move(done)));
            return;
        }
        const std::optional<Ownership> owner =
            registry_.known_owner(room).or_else([&] { return it->second.owner; });
        if (!owner || owner->node == config_.self) {
            done(std::unexpected(RouteError::Unavailable));
            return;
        }
        std::vector<std::byte> frame;
        const std::uint64_t id = next_request_++;
        wire::encode_ask(frame, id, room, request);
        ++counters_.forwarded;
        link(owner->node)
            .request(
                id, frame,
                [this, room, done = asking(room, from, std::move(done))](
                    wire::Status status, std::uint64_t /*seq*/,
                    std::span<const std::byte> body) mutable noexcept {
                    if (status != wire::Status::Ok) {
                        if (status == wire::Status::NotOwner) {
                            lost_owner(room);
                        }
                        done(std::unexpected(route_error(status)));
                        return;
                    }
                    try {
                        done(std::vector<std::byte>(body.begin(), body.end()));
                    } catch (const std::bad_alloc&) {
                        ++counters_.allocation_failures;
                        done(std::unexpected(RouteError::Unavailable));
                    }
                },
                kOwnerAskTimeout);
    }

    void serve(IOwnerService* service) noexcept { service_ = service; }

    // ---- notices

    void notify(const core::RoomId& room, std::span<const std::byte> body) noexcept {
        if (body.size() > kMaxOwnerMessage) {
            ++counters_.notices_dropped;
            return;
        }
        bool looking = false;
        try {
            if (registry_.owned(room)) {
                fan_notice(room, body);
                return;
            }
            if (const auto owner = known_owner_of(room)) {
                forward_notice(owner->node, room, body);
                return;
            }
            // Read, not resolved: a lookup that found no owner would claim the room, and a room
            // nobody joined has nobody to hear the notice.
            auto [it, first] = notice_lookups_.try_emplace(room);
            if (first && notice_lookups_.size() > kMaxNoticeLookups) {
                notice_lookups_.erase(it);
                ++counters_.notices_dropped;
                return;
            }
            if (it->second.size() >= kMaxNoticesPerLookup) {
                ++counters_.notices_dropped;
                return;
            }
            it->second.emplace_back(body.begin(), body.end());
            if (!first) {
                return;
            }
            looking = true;
            store_.read_owners(
                {room},
                [this, room](
                    StoreResult<std::vector<std::pair<core::RoomId, Ownership>>> owners) noexcept {
                    try {
                        looked_up(room, owners);
                    } catch (const std::bad_alloc&) {
                        ++counters_.allocation_failures;
                        ++counters_.notices_dropped;
                    }
                });
        } catch (const std::bad_alloc&) {
            ++counters_.allocation_failures;
            ++counters_.notices_dropped;
            // Whatever waits for a lookup that never started would wait for good.
            if (looking) {
                if (const auto it = notice_lookups_.find(room); it != notice_lookups_.end()) {
                    counters_.notices_dropped += it->second.size();
                    notice_lookups_.erase(it);
                }
            }
        }
    }

    void hear(INoticeListener* listener) noexcept { listener_ = listener; }

    [[nodiscard]] bool owns(const core::RoomId& room) const noexcept {
        return registry_.owned(room).has_value();
    }

    // The node a notice of a room goes to: this one's idea of its owner, if it has one.
    [[nodiscard]] std::optional<Ownership> known_owner_of(const core::RoomId& room) const {
        if (auto known = registry_.known_owner(room)) {
            return known;
        }
        const auto it = local_.find(room);
        if (it != local_.end() && remote_owner(it->second)) {
            return it->second.owner;
        }
        return std::nullopt;
    }

    void looked_up(const core::RoomId& room,
                   const StoreResult<std::vector<std::pair<core::RoomId, Ownership>>>& owners) {
        const auto it = notice_lookups_.find(room);
        if (it == notice_lookups_.end()) {
            return;
        }
        const std::vector<std::vector<std::byte>> waiting = std::move(it->second);
        notice_lookups_.erase(it);
        std::optional<Ownership> owner;
        if (owners) {
            for (const auto& [r, o] : *owners) {
                if (r == room) {
                    owner = o;
                }
            }
        }
        for (const std::vector<std::byte>& body : waiting) {
            if (registry_.owned(room)) {
                fan_notice(room, body);
            } else if (owner && owner->node != config_.self) {
                forward_notice(owner->node, room, body);
            } else {
                ++counters_.notices_dropped;
            }
        }
    }

    void forward_notice(const core::NodeId& node, const core::RoomId& room,
                        std::span<const std::byte> body) {
        std::vector<std::byte> frame;
        wire::encode_notify(frame, room, body);
        ++counters_.notices_forwarded;
        link(node).send(frame);
    }

    // As the room's owner: to its members here, and to every node subscribed to it.
    void fan_notice(const core::RoomId& room, std::span<const std::byte> body) {
        ++counters_.notices_fanned_out;
        hear_here(room, body);
        const auto o = owned_.find(room);
        if (o == owned_.end() || o->second.subscribers.empty()) {
            return;
        }
        std::vector<std::byte> frame;
        wire::encode_notice(frame, room, body);
        std::erase_if(o->second.subscribers, [&](const net::Slab<Inbound>::Handle& h) {
            Inbound* in = inbound_.get(h);
            if (in == nullptr) {
                return true;
            }
            in->send(frame);
            return false;
        });
    }

    void hear_here(const core::RoomId& room, std::span<const std::byte> body) noexcept {
        if (listener_ == nullptr || !local_.contains(room)) {
            return;
        }
        ++counters_.notices_heard;
        listener_->on_notice(room, body);
    }

    // A Notify from another node: passed on if this node holds the room, else dropped. Never
    // resolved, which could take a room nobody here has members in.
    void on_notify(const core::RoomId& room, std::span<const std::byte> body) {
        if (!registry_.owned(room) || body.size() > kMaxOwnerMessage) {
            ++counters_.notices_dropped;
            return;
        }
        fan_notice(room, body);
    }

    // Answers the asks past their deadline Unavailable, once they are out of asks_: an answer
    // may ask again.
    void expire_asks(core::MonoTime now) {
        std::vector<OwnerAnswer> late;
        std::erase_if(asks_, [&](auto& entry) {
            if (entry.second.deadline > now) {
                return false;
            }
            late.push_back(std::move(entry.second.done));
            return true;
        });
        counters_.ask_timeouts += late.size();
        for (OwnerAnswer& done : late) {
            done(std::unexpected(RouteError::Unavailable));
        }
    }

    // Holds `done` for an ask by `member`, as pending() holds a send's.
    OwnerAnswer asking(const core::RoomId& room, IMember& member, OwnerAnswer done) {
        const std::uint64_t id = next_send_++;
        asks_.emplace(id, PendingAsk{.room = room,
                                     .member = &member,
                                     .done = std::move(done),
                                     .deadline = clock_.now() + kOwnerAskTimeout});
        return [this, id](std::expected<std::vector<std::byte>, RouteError> result) noexcept {
            const auto it = asks_.find(id);
            if (it == asks_.end()) {
                return;
            }
            OwnerAnswer answer = std::move(it->second.done);
            asks_.erase(it);
            answer(std::move(result));
        };
    }

    // This node owns the room: its service answers, or nobody does.
    void answer_here(const core::RoomId& room, std::span<const std::byte> request,
                     OwnerAnswer done) {
        if (service_ == nullptr) {
            done(std::unexpected(RouteError::Unavailable));
            return;
        }
        service_->on_ask(
            room, request,
            [done = std::move(done)](
                std::expected<std::vector<std::byte>, RouteError> result) mutable noexcept {
                if (result && result->size() > kMaxOwnerMessage) {
                    result = std::unexpected(RouteError::Unavailable);
                }
                done(std::move(result));
            });
    }

    void release_rooms(StoreCallback<void> done) {
        // From here on this node takes no room, not even one it had: it is going away.
        draining_ = true;
        std::unordered_map<core::RoomId, OwnedRoomState> released = std::move(owned_);
        owned_.clear();
        for (auto& [room, lr] : local_) {
            if (lr.owner && lr.owner->node == config_.self) {
                lr.owner.reset();
            }
        }
        registry_.release_all(std::move(done));
        for (auto& [room, o] : released) {
            disown(room, o, RouteError::Unavailable);
        }
    }

    void reap() noexcept {
        inbound_.reap([this](Inbound& in) { return reactor_.is_quiescent(in.conn()); });
        std::erase_if(closing_, [this](const std::unique_ptr<Outbound>& link) {
            return link->quiescent(reactor_);
        });
    }

    [[nodiscard]] bool healthy() const noexcept { return advertised_ && registry_.healthy(); }
    [[nodiscard]] std::size_t rooms_owned() const noexcept { return registry_.rooms_owned(); }
    [[nodiscard]] std::size_t rooms_joined() const noexcept { return local_.size(); }
    [[nodiscard]] const RegistryCounters& registry_counters() const noexcept {
        return registry_.counters();
    }
    [[nodiscard]] const RouterCounters& counters() const noexcept { return counters_; }

    // ---- registry

    void on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept override {
        const auto mine = registry_.owned(room);
        if (owner.node == config_.self && mine == owner.generation) {
            events_.on_took_room(room, owner.generation);
        }
        const auto it = local_.find(room);
        if (it == local_.end()) {
            return;
        }
        LocalRoom& lr = it->second;
        if (mine) {
            settle(room, lr, Ownership{.node = config_.self, .generation = *mine});
            return;
        }
        if (owner.node == config_.self || lr.owner == owner || lr.subscribing) {
            return;
        }
        subscribe(room, owner);
    }

    void on_fenced_out(const core::RoomId& room, std::uint64_t generation,
                       OwnerWrite write) noexcept override {
        events_.on_fenced_out(room, generation, write);
        // Taken again under a newer generation since that write went out: the room is still
        // this node's, and so are its queue and subscribers.
        if (registry_.owned(room)) {
            return;
        }
        if (const auto it = owned_.find(room); it != owned_.end()) {
            OwnedRoomState gone = std::move(it->second);
            owned_.erase(it);
            // The write being appended is the fenced one only if the append said so; after a
            // fenced heartbeat its own outcome is unknown, and it may well have committed.
            disown(room, gone, RouteError::Fenced,
                   write == OwnerWrite::Append ? RouteError::Fenced : RouteError::Unavailable);
        }
        const auto it = local_.find(room);
        if (it == local_.end()) {
            return;
        }
        LocalRoom& lr = it->second;
        if (lr.owner && lr.owner->node == config_.self) {
            lr.owner.reset();
            find_owner(room);
        }
    }

    // ---- node channel

    void on_accept(os::UniqueFd conn) noexcept override {
        [[maybe_unused]] const auto tuned = net::tune_connection(conn.get());
        make_room_for_a_handshake();
        const auto handle = inbound_.emplace(*this);
        if (!handle) {
            return;
        }
        Inbound* in = inbound_.get(*handle);
        auto id = reactor_.attach(std::move(conn), *in);
        if (!id) {
            inbound_.retire(*handle);
            return;
        }
        in->start(*id);
    }

    [[nodiscard]] std::size_t authenticated_peers() noexcept {
        std::size_t n = 0;
        inbound_.for_each_live([&](const Inbound& in) {
            if (in.authenticated()) {
                ++n;
            }
        });
        return n;
    }

    void make_room_for_a_handshake() noexcept {
        inbound_.reap([this](Inbound& in) { return reactor_.is_quiescent(in.conn()); });
        std::size_t handshaking = 0;
        Inbound* oldest = nullptr;
        inbound_.for_each_live([&](Inbound& in) {
            if (in.authenticated()) {
                return;
            }
            ++handshaking;
            if (oldest == nullptr || in.accepted() < oldest->accepted()) {
                oldest = &in;
            }
        });
        if (handshaking >= kMaxHandshaking && oldest != nullptr) {
            ++counters_.handshakes_evicted;
            oldest->close();
        }
    }

    void on_timeout() noexcept override {
        timer_ = reactor_.arm_timer(kTick, *this);
        tick();
    }

private:
    struct Joining {
        IMember* member;
        JoinCallback done;
    };

    struct PendingSend {
        core::RoomId room;
        IMember* member;
        SendCallback done;
    };

    struct PendingAsk {
        core::RoomId room;
        IMember* member;
        OwnerAnswer done;
        // Answered Unavailable then, whoever was to answer: the owner's own service, which may
        // be this node's, or another node over the channel.
        core::MonoTime deadline;
    };

    // A room with members on this node.
    struct LocalRoom {
        std::vector<IMember*> members;
        std::vector<Joining> joining;
        // Where the room's messages come from: this node, or the node that took this one's
        // subscription. Unset while that is being found.
        std::optional<Ownership> owner;
        bool subscribing = false;
        // Deliveries can repeat after a resubscription; each seq reaches the members once.
        std::uint64_t delivered = 0;
        // The owner's head when it took this node's subscription.
        std::uint64_t head = 0;
    };

    // A write waiting for its sequence number, from a member here or from another node.
    struct Write {
        core::UserId sender;
        MessageKey key;
        std::vector<std::byte> body;
        SendCallback local;
        std::optional<net::Slab<Inbound>::Handle> peer;
        std::uint64_t request = 0;
    };

    // A room this node owns: the nodes that subscribed, and the writes in sequence order. The
    // front write is the one being appended.
    struct OwnedRoomState {
        std::vector<net::Slab<Inbound>::Handle> subscribers;
        std::deque<Write> writes;
        std::size_t queued_bytes = 0;
        bool appending = false;
        // The last seq this node took for the room.
        std::uint64_t head = 0;
    };

    // The latest seq known here: taken as the owner, delivered, or named by the owner.
    [[nodiscard]] std::uint64_t head(const core::RoomId& room) const noexcept {
        std::uint64_t latest = 0;
        if (const auto o = owned_.find(room); o != owned_.end()) {
            latest = o->second.head;
        }
        // Taken over, its head is where the store's count stood, not the 0 of a fresh queue:
        // a member joining now must see what the old owner sequenced as behind it.
        if (registry_.owned(room)) {
            latest = std::max(latest, registry_.taken_at(room));
        }
        if (const auto l = local_.find(room); l != local_.end()) {
            latest = std::max({latest, l->second.delivered, l->second.head});
        }
        return latest;
    }

    static std::size_t cost(const Write& write) noexcept {
        return write.body.size() + kWriteOverhead;
    }

    void tick() {
        ++counters_.ticks;
        const core::MonoTime now = clock_.now();
        inbound_.for_each_live([&](Inbound& in) {
            if (in.handshake_overdue(now)) {
                in.close();
            } else if (in.stalled(now, config_.peer_stall_timeout)) {
                in.cut_off();
            }
        });
        std::vector<Outbound*> stuck;
        for (auto& [node, link] : outbound_) {
            if (link->stalled(now, config_.peer_stall_timeout)) {
                link->cut_off();
            }
            if (link->due_down(now)) {
                stuck.push_back(link.get());
            }
        }
        for (Outbound* link : stuck) {
            link_down(*link);
        }
        std::vector<RequestDone> expired;
        for (auto& [node, link] : outbound_) {
            link->expire(now, expired);
        }
        counters_.forward_timeouts += expired.size();
        for (RequestDone& done : expired) {
            done(wire::Status::Unavailable, 0, {});
        }
        expire_asks(now);
        if (now < next_beat_) {
            return;
        }
        next_beat_ = now + kOwnerHeartbeat;
        if (draining_) {
            return;
        }
        // Nothing is claimed before this run has advertised: until then the store cannot tell
        // this run's claims from an earlier run's, nor this process from another under its name.
        if (advertised_) {
            registry_.tick();
        }
        if (!advertised_ && !advertising_) {
            advertise();
        }
        // Rooms whose owner could not be reached are tried again, once a beat.
        find_orphans();
        release_idle(now);
        if (now >= next_revalidation_) {
            next_revalidation_ = now + config_.revalidate_every;
            revalidate();
        }
    }

    void release_idle(core::MonoTime now) {
        for (const core::RoomId& room : registry_.owned_rooms()) {
            const auto o = owned_.find(room);
            // A subscriber whose connection has closed is gone; only fan-out would otherwise
            // notice, and a quiet room has none.
            if (o != owned_.end()) {
                std::erase_if(o->second.subscribers, [this](const net::Slab<Inbound>::Handle& h) {
                    return inbound_.get(h) == nullptr;
                });
            }
            const bool used = local_.contains(room) ||
                              (o != owned_.end() &&
                               (!o->second.subscribers.empty() || !o->second.writes.empty()));
            if (used) {
                idle_since_.erase(room);
                continue;
            }
            const auto [since, first] = idle_since_.try_emplace(room, now);
            if (first || now - since->second < config_.idle_release) {
                continue;
            }
            idle_since_.erase(since);
            if (o != owned_.end()) {
                owned_.erase(o);
            }
            registry_.release(room);
        }
        std::erase_if(idle_since_,
                      [this](const auto& entry) { return !registry_.owned(entry.first); });
    }

    // Notifications are hints and a listening session can miss them; the owners of every room
    // routed to another node are read again now and then, in one read-only statement, and an
    // owner newer than the one known is subscribed to like one a notification named.
    void revalidate() {
        std::vector<core::RoomId> rooms;
        for (const auto& [room, lr] : local_) {
            if (remote_owner(lr) && !lr.subscribing) {
                rooms.push_back(room);
            }
        }
        if (rooms.empty() || revalidating_) {
            return;
        }
        revalidating_ = true;
        store_.read_owners(
            std::move(rooms),
            [this](StoreResult<std::vector<std::pair<core::RoomId, Ownership>>> owners) noexcept {
                revalidating_ = false;
                if (!owners) {
                    return;
                }
                for (const auto& [room, owner] : *owners) {
                    registry_.on_owner_changed(room, owner);
                }
            });
    }

    void find_orphans() {
        std::vector<core::RoomId> orphans;
        for (const auto& [room, lr] : local_) {
            if (!lr.owner && !lr.subscribing) {
                orphans.push_back(room);
            }
        }
        for (const core::RoomId& room : orphans) {
            find_owner(room);
        }
    }

    // ---- finding owners and subscribing

    // Not before this node has advertised: the store tells this run's claims from an earlier
    // run's by the time it last did, and would otherwise take this run's own rooms again.
    void find_owner(const core::RoomId& room) {
        const auto found = local_.find(room);
        if (found == local_.end() || found->second.subscribing || draining_ || !advertised_) {
            return;
        }
        LocalRoom& lr = found->second;
        if (const auto mine = registry_.owned(room)) {
            settle(room, lr, Ownership{.node = config_.self, .generation = *mine});
            return;
        }
        if (const auto known = registry_.known_owner(room)) {
            subscribe(room, *known);
            return;
        }
        lr.subscribing = true;
        registry_.resolve(room, [this, room](StoreResult<Ownership> owner) noexcept {
            const auto it = local_.find(room);
            if (it == local_.end()) {
                return;
            }
            LocalRoom& now = it->second;
            now.subscribing = false;
            if (!owner) {
                fail_joins(room, RouteError::Unavailable);
                return;
            }
            if (const auto mine = registry_.owned(room)) {
                settle(room, now, Ownership{.node = config_.self, .generation = *mine});
                return;
            }
            subscribe(room, *owner);
        });
    }

    void subscribe(const core::RoomId& room, const Ownership& owner) {
        const auto it = local_.find(room);
        if (it == local_.end()) {
            return;
        }
        it->second.subscribing = true;
        std::vector<std::byte> frame;
        const std::uint64_t request = next_request_++;
        wire::encode_subscribe(frame, request, room);
        link(owner.node)
            .request(request, frame,
                     [this, room, owner](wire::Status status, std::uint64_t seq,
                                         std::span<const std::byte> /*body*/) noexcept {
                         subscribed(room, owner, status, seq);
                     });
    }

    // `seq`, when the owner took the subscription, is its head.
    void subscribed(const core::RoomId& room, const Ownership& owner, wire::Status status,
                    std::uint64_t seq) {
        const auto it = local_.find(room);
        if (it == local_.end()) {
            if (status == wire::Status::Ok) {
                unsubscribe(room, owner.node);
            }
            return;
        }
        LocalRoom& lr = it->second;
        lr.subscribing = false;
        if (status != wire::Status::Ok) {
            if (status == wire::Status::NotOwner) {
                registry_.forget(room);
            }
            fail_joins(room, RouteError::Unavailable);
            return;
        }
        lr.head = std::max(lr.head, seq);
        settle(room, lr, owner);
        // The owner changed hands while the subscription was on its way.
        const auto known = registry_.known_owner(room);
        if (known && known != owner && known->node != config_.self) {
            subscribe(room, *known);
        }
    }

    // The other node the room's messages come from, if they come from one.
    [[nodiscard]] std::optional<core::NodeId> remote_owner(const LocalRoom& lr) const {
        if (!lr.owner.has_value() || lr.owner->node == config_.self) {
            return std::nullopt;
        }
        return lr.owner->node;
    }

    // Messages now come from `owner`; the joins waiting for that are done.
    void settle(const core::RoomId& room, LocalRoom& lr, const Ownership& owner) {
        if (const auto before = remote_owner(lr); before && *before != owner.node) {
            unsubscribe(room, *before);
        }
        lr.owner = owner;
        std::vector<Joining> joined = std::move(lr.joining);
        lr.joining.clear();
        // A member may have asked twice before the first was answered; it joins once.
        for (const Joining& j : joined) {
            if (std::ranges::find(lr.members, j.member) == lr.members.end()) {
                lr.members.push_back(j.member);
            }
        }
        const std::uint64_t latest = head(room);
        // Its members are told the head; a seq at or below it that arrives later (a repeat an
        // owner that knows the room less well delivers again) is one they have or can fetch.
        lr.delivered = std::max(lr.delivered, latest);
        for (Joining& j : joined) {
            j.done(latest);
        }
    }

    void fail_joins(const core::RoomId& room, RouteError error) {
        const auto it = local_.find(room);
        if (it == local_.end()) {
            return;
        }
        std::vector<Joining> failed = std::move(it->second.joining);
        it->second.joining.clear();
        drop_if_empty(room);
        for (Joining& j : failed) {
            j.done(std::unexpected(error));
        }
    }

    void drop_if_empty(const core::RoomId& room) {
        const auto it = local_.find(room);
        if (it == local_.end() || !it->second.members.empty() || !it->second.joining.empty()) {
            return;
        }
        if (const auto owner = remote_owner(it->second)) {
            unsubscribe(room, *owner);
        }
        local_.erase(it);
        registry_.set_interest(room, false);
    }

    void unsubscribe(const core::RoomId& room, const core::NodeId& node) {
        const auto it = outbound_.find(node);
        if (it == outbound_.end()) {
            return;
        }
        std::vector<std::byte> frame;
        wire::encode_unsubscribe(frame, room);
        it->second->send(frame);
    }

    // The owner `node` let the room go; find where it went now, not at the next beat.
    void disowned(const core::RoomId& room, const core::NodeId& node) {
        const auto it = local_.find(room);
        if (it == local_.end() || remote_owner(it->second) != node) {
            return;
        }
        lost_owner(room);
        find_owner(room);
    }

    // The node this one routed the room to is not its owner, or not reachable.
    void lost_owner(const core::RoomId& room) {
        registry_.forget(room);
        const auto it = local_.find(room);
        if (it != local_.end() && remote_owner(it->second)) {
            it->second.owner.reset();
        }
    }

    // ---- sequencing, as the owner

    void enqueue(const core::RoomId& room, Write write) {
        OwnedRoomState& o = owned_[room];
        const std::size_t bytes = cost(write);
        if (o.queued_bytes + bytes > kMaxRoomQueueBytes || queued_bytes_ + bytes > kMaxQueueBytes) {
            answer(write, std::unexpected(RouteError::Busy));
            return;
        }
        o.queued_bytes += bytes;
        queued_bytes_ += bytes;
        o.writes.push_back(std::move(write));
        pump(room);
    }

    void pump(const core::RoomId& room) {
        // A retry queued behind its first try is answered as soon as that one is sequenced,
        // from the same queue: the queue is in order, so the first try always comes first.
        auto it = owned_.find(room);
        while (it != owned_.end() && !it->second.appending && !it->second.writes.empty()) {
            OwnedRoomState& o = it->second;
            const auto seen = recent_.find(room, o.writes.front().sender, o.writes.front().key);
            if (!seen) {
                break;
            }
            Write retry = std::move(o.writes.front());
            o.writes.pop_front();
            o.queued_bytes -= cost(retry);
            queued_bytes_ -= cost(retry);
            if (seen->digest != RecentKeys::digest(retry.body)) {
                answer(retry, std::unexpected(RouteError::Conflict));
            } else {
                ++counters_.duplicates;
                answer(retry, seen->seq);
            }
            it = owned_.find(room);
        }
        if (it == owned_.end() || it->second.appending || it->second.writes.empty()) {
            return;
        }
        it->second.appending = true;
        const Write& next = it->second.writes.front();
        const bool started = registry_.append(
            room, Outgoing{.sender = next.sender, .key = next.key, .body = next.body},
            [this, room](std::expected<std::uint64_t, AppendError> seq) noexcept {
                appended(room, seq);
            });
        // Not owned any more, and not through a fence either: the rooms were released.
        if (!started) {
            OwnedRoomState gone = std::move(it->second);
            owned_.erase(it);
            disown(room, gone, RouteError::Unavailable);
        }
    }

    void appended(const core::RoomId& room, std::expected<std::uint64_t, AppendError> seq) {
        const auto it = owned_.find(room);
        // Gone: the room was fenced out, and every write it held has been answered.
        if (it == owned_.end()) {
            return;
        }
        OwnedRoomState& o = it->second;
        Write write = std::move(o.writes.front());
        o.writes.pop_front();
        o.queued_bytes -= cost(write);
        queued_bytes_ -= cost(write);
        o.appending = false;
        if (!seq) {
            answer(write, std::unexpected(route_error(seq.error())));
        } else if (*seq <= std::max(o.head, registry_.taken_at(room))) {
            // A repeat the store recognised by its key: sequenced before, under this seq, and
            // delivered then, by this node or by the owner it took the room from (whose count
            // stood at taken_at). It is remembered for the next repeat, and not delivered again.
            recent_.remember(room, write.sender, write.key,
                             {.seq = *seq, .digest = RecentKeys::digest(write.body)}, clock_.now());
            answer(write, *seq);
        } else {
            o.head = *seq;
            fan_out(room, o, *seq, write);
            answer(write, *seq);
        }
        pump(room);
    }

    void fan_out(const core::RoomId& room, OwnedRoomState& o, std::uint64_t seq,
                 const Write& write) {
        recent_.remember(room, write.sender, write.key,
                         {.seq = seq, .digest = RecentKeys::digest(write.body)}, clock_.now());
        deliver_here(Message{.room = room,
                             .seq = seq,
                             .sender = write.sender,
                             .key = write.key,
                             .body = write.body});
        if (o.subscribers.empty()) {
            return;
        }
        std::vector<std::byte> frame;
        wire::encode_deliver(frame, room, seq, write.sender, write.key, write.body);
        std::erase_if(o.subscribers, [&](const net::Slab<Inbound>::Handle& h) {
            Inbound* in = inbound_.get(h);
            if (in == nullptr) {
                return true;
            }
            in->send(frame);
            return false;
        });
    }

    void deliver_here(const Message& message) {
        const auto it = local_.find(message.room);
        if (it == local_.end() || message.seq <= it->second.delivered) {
            return;
        }
        it->second.delivered = message.seq;
        ++counters_.delivered;
        for (IMember* member : it->second.members) {
            member->deliver(message);
        }
    }

    void answer(Write& write, std::expected<std::uint64_t, RouteError> result) {
        if (write.local) {
            write.local(result);
            return;
        }
        if (!write.peer) {
            return;
        }
        Inbound* in = inbound_.get(*write.peer);
        if (in == nullptr) {
            return;
        }
        std::vector<std::byte> frame;
        wire::encode_reply(frame, write.request,
                           result ? wire::Status::Ok : wire_status(result.error()),
                           result.value_or(0));
        in->send(frame);
    }

    // This node no longer owns the room: its writes are answered, and the nodes subscribed to
    // it are told to look for the new owner rather than wait for deliveries that never come.
    void disown(const core::RoomId& room, OwnedRoomState& o, RouteError error,
                std::optional<RouteError> appending_error = std::nullopt) {
        std::vector<std::byte> frame;
        wire::encode_unsubscribe(frame, room);
        for (const net::Slab<Inbound>::Handle& h : o.subscribers) {
            if (Inbound* in = inbound_.get(h)) {
                in->send(frame);
            }
        }
        fail_writes(o, error, appending_error);
    }

    // `appending_error`, when given, answers the write whose append is in flight.
    void fail_writes(OwnedRoomState& o, RouteError error,
                     std::optional<RouteError> appending_error = std::nullopt) {
        std::deque<Write> writes = std::move(o.writes);
        o.writes.clear();
        queued_bytes_ -= o.queued_bytes;
        o.queued_bytes = 0;
        for (std::size_t i = 0; i < writes.size(); ++i) {
            const bool in_flight = i == 0 && o.appending;
            answer(writes[i], std::unexpected(in_flight ? appending_error.value_or(error) : error));
        }
    }

    // ---- requests from other nodes, as the owner

    void on_subscribe(net::Slab<Inbound>::Handle peer, std::uint64_t request,
                      const core::RoomId& room) {
        if (registry_.owned(room)) {
            add_subscriber(room, peer, request);
            return;
        }
        if (draining_) {
            reply(peer, request, wire::Status::NotOwner, 0);
            return;
        }
        if (!advertised_) {
            reply(peer, request, wire::Status::Unavailable, 0);
            return;
        }
        // The dialer found this node in the store. If this node does not hold the room it
        // may be a restart under the same name, which the lookup takes again.
        registry_.resolve(room, [this, peer, request, room](StoreResult<Ownership>) noexcept {
            if (registry_.owned(room)) {
                add_subscriber(room, peer, request);
                return;
            }
            reply(peer, request, wire::Status::NotOwner, 0);
        });
    }

    void add_subscriber(const core::RoomId& room, net::Slab<Inbound>::Handle peer,
                        std::uint64_t request) {
        auto& subscribers = owned_[room].subscribers;
        if (std::ranges::find(subscribers, peer) == subscribers.end()) {
            subscribers.push_back(peer);
        }
        reply(peer, request, wire::Status::Ok, head(room));
    }

    void on_unsubscribe(net::Slab<Inbound>::Handle peer, const core::RoomId& room) noexcept {
        const auto it = owned_.find(room);
        if (it != owned_.end()) {
            std::erase(it->second.subscribers, peer);
        }
    }

    void on_forwarded(net::Slab<Inbound>::Handle peer, std::uint64_t request,
                      const core::RoomId& room, const core::UserId& sender, const MessageKey& key,
                      std::span<const std::byte> body) {
        Write write{.sender = sender,
                    .key = key,
                    .body = {body.begin(), body.end()},
                    .local = {},
                    .peer = peer,
                    .request = request};
        if (registry_.owned(room)) {
            enqueue(room, std::move(write));
            return;
        }
        if (draining_) {
            reply(peer, request, wire::Status::NotOwner, 0);
            return;
        }
        if (!advertised_) {
            reply(peer, request, wire::Status::Unavailable, 0);
            return;
        }
        registry_.resolve(room, [this, peer, request, room, write = std::move(write)](
                                    StoreResult<Ownership> owner) mutable noexcept {
            if (registry_.owned(room)) {
                enqueue(room, std::move(write));
                return;
            }
            // Another node owns it: the sender's route is stale, and it must hear so.
            reply(peer, request, owner ? wire::Status::NotOwner : wire::Status::Unavailable, 0);
        });
    }

    void on_ask(net::Slab<Inbound>::Handle peer, std::uint64_t request, const core::RoomId& room,
                std::span<const std::byte> body) {
        if (registry_.owned(room)) {
            answer_ask(peer, request, room, body);
            return;
        }
        if (draining_) {
            answer(peer, request, wire::Status::NotOwner, {});
            return;
        }
        if (!advertised_) {
            answer(peer, request, wire::Status::Unavailable, {});
            return;
        }
        // As for a forwarded send: the asker found this node in the store, and a restart under
        // the same name takes the room again.
        registry_.resolve(room, [this, peer, request, room,
                                 copy = std::vector<std::byte>(body.begin(), body.end())](
                                    StoreResult<Ownership> owner) noexcept {
            if (registry_.owned(room)) {
                answer_ask(peer, request, room, copy);
                return;
            }
            answer(peer, request, owner ? wire::Status::NotOwner : wire::Status::Unavailable, {});
        });
    }

    void answer_ask(net::Slab<Inbound>::Handle peer, std::uint64_t request,
                    const core::RoomId& room, std::span<const std::byte> body) {
        if (body.size() > kMaxOwnerMessage) {
            answer(peer, request, wire::Status::Unavailable, {});
            return;
        }
        answer_here(room, body,
                    [this, peer,
                     request](std::expected<std::vector<std::byte>, RouteError> result) noexcept {
                        if (!result) {
                            answer(peer, request, wire_status(result.error()), {});
                            return;
                        }
                        answer(peer, request, wire::Status::Ok, *result);
                    });
    }

    void answer(net::Slab<Inbound>::Handle peer, std::uint64_t request, wire::Status status,
                std::span<const std::byte> body) noexcept {
        Inbound* in = inbound_.get(peer);
        if (in == nullptr) {
            return;
        }
        try {
            std::vector<std::byte> frame;
            wire::encode_answer(frame, request, status, body);
            in->send(frame);
        } catch (const std::bad_alloc&) {
            // The asker times out and says Unavailable, as for a lost answer.
            ++counters_.allocation_failures;
        }
    }

    void reply(net::Slab<Inbound>::Handle peer, std::uint64_t request, wire::Status status,
               std::uint64_t seq) {
        Inbound* in = inbound_.get(peer);
        if (in == nullptr) {
            return;
        }
        std::vector<std::byte> frame;
        wire::encode_reply(frame, request, status, seq);
        in->send(frame);
    }

    // ---- deliveries from owners, as a member node

    // Whichever node sends it, a delivery was sequenced under a fence, so it is genuine; the
    // seq check drops what a resubscription repeats. Its key is remembered for whichever node
    // owns the room when the sender tries again through this one.
    void on_deliver(const Message& message) {
        recent_.remember(message.room, message.sender, message.key,
                         {.seq = message.seq, .digest = RecentKeys::digest(message.body)},
                         clock_.now());
        deliver_here(message);
    }

    // ---- links to other nodes

    Outbound& link(const core::NodeId& node) {
        const auto it = outbound_.find(node);
        if (it != outbound_.end()) {
            return *it->second;
        }
        auto made = std::make_unique<Outbound>(*this, node, next_link_++);
        Outbound& out = *made;
        outbound_.emplace(node, std::move(made));
        out.locate();
        return out;
    }

    void link_down(Outbound& dead) noexcept {
        const core::NodeId node = dead.node();
        const auto it = outbound_.find(node);
        if (it == outbound_.end() || it->second.get() != &dead) {
            return;
        }
        std::unique_ptr<Outbound> gone = std::move(it->second);
        outbound_.erase(it);
        ++counters_.peers_lost;
        events_.on_peer_lost(node);
        for (auto& [room, lr] : local_) {
            if (lr.owner && lr.owner->node == node) {
                lr.owner.reset();
                registry_.forget(room);
            }
        }
        gone->close();
        // Fails what was waiting on the link; the callbacks may open a new one.
        gone->fail_requests();
        closing_.push_back(std::move(gone));
    }

    [[nodiscard]] std::span<const std::byte> secret() const noexcept {
        return std::as_bytes(std::span{config_.secret});
    }

    void refused(std::string_view why) noexcept {
        ++counters_.peers_refused;
        events_.on_peer_refused(why);
    }

    void advertise() {
        advertising_ = true;
        store_.advertise(config_.self, config_.advertise, incarnation_,
                         [this](StoreResult<void> r) noexcept {
                             advertising_ = false;
                             advertised_ = r.has_value();
                             const bool taken = !r && r.error() == StoreError::NodeTaken;
                             if (taken && !node_taken_) {
                                 events_.on_node_taken();
                             }
                             node_taken_ = taken;
                             if (advertised_) {
                                 find_orphans();
                             }
                         });
    }

    net::IReactor& reactor_;
    IRoomStore& store_;
    const core::ports::IClock& clock_;
    core::ports::IRandom& random_;
    RouterConfig config_;
    IRouterEvents& events_;
    core::Uuid incarnation_;
    RoomRegistry registry_;
    RecentKeys recent_;
    std::unordered_map<core::RoomId, LocalRoom> local_;
    std::unordered_map<core::RoomId, OwnedRoomState> owned_;
    std::size_t queued_bytes_ = 0;
    // Owned rooms nobody here uses, and since when.
    std::unordered_map<core::RoomId, core::MonoTime> idle_since_;
    net::Slab<Inbound> inbound_;
    std::unordered_map<core::NodeId, std::unique_ptr<Outbound>> outbound_;
    std::vector<std::unique_ptr<Outbound>> closing_;
    std::uint64_t next_request_ = 1;
    // Sends of members here not answered yet, by their own id.
    std::unordered_map<std::uint64_t, PendingSend> sends_;
    // Asks of members here not answered yet, numbered from next_send_ too.
    std::unordered_map<std::uint64_t, PendingAsk> asks_;
    std::uint64_t next_send_ = 1;
    IOwnerService* service_ = nullptr;
    INoticeListener* listener_ = nullptr;
    // Notices waiting for their room's owner to be read, by room.
    std::unordered_map<core::RoomId, std::vector<std::vector<std::byte>>> notice_lookups_;
    std::uint64_t next_link_ = 1;
    net::TimerId timer_;
    core::MonoTime next_beat_;
    core::MonoTime next_revalidation_;
    bool advertised_ = false;
    bool draining_ = false;
    bool node_taken_ = false;
    bool revalidating_ = false;
    bool advertising_ = false;
    RouterCounters counters_;
};

RoomRouter::RoomRouter(net::IReactor& reactor, IRoomStore& store, const core::ports::IClock& clock,
                       core::ports::IRandom& random, RouterConfig config, IRouterEvents& events)
    : impl_(std::make_unique<Impl>(reactor, store, clock, random, std::move(config), events)) {}

RoomRouter::~RoomRouter() = default;

std::expected<void, int> RoomRouter::start(os::UniqueFd listener) {
    return impl_->start(std::move(listener));
}

void RoomRouter::join(const core::RoomId& room, IMember& member, JoinCallback done) {
    impl_->join(room, member, std::move(done));
}

void RoomRouter::leave(const core::RoomId& room, IMember& member) noexcept {
    impl_->leave(room, member);
}

void RoomRouter::send(const core::RoomId& room, IMember& from, const core::UserId& sender,
                      const MessageKey& key, std::vector<std::byte> body, SendCallback done) {
    impl_->send(room, from, sender, key, std::move(body), std::move(done));
}

void RoomRouter::ask_owner(const core::RoomId& room, IMember& from,
                           std::span<const std::byte> request, OwnerAnswer done) {
    impl_->ask_owner(room, from, request, std::move(done));
}

void RoomRouter::serve(IOwnerService* service) noexcept {
    impl_->serve(service);
}

void RoomRouter::notify(const core::RoomId& room, std::span<const std::byte> body) noexcept {
    impl_->notify(room, body);
}

void RoomRouter::hear(INoticeListener* listener) noexcept {
    impl_->hear(listener);
}

bool RoomRouter::owns(const core::RoomId& room) const noexcept {
    return impl_->owns(room);
}

void RoomRouter::release_rooms(StoreCallback<void> done) {
    impl_->release_rooms(std::move(done));
}

void RoomRouter::reap() noexcept {
    impl_->reap();
}

bool RoomRouter::healthy() const noexcept {
    return impl_->healthy();
}

std::size_t RoomRouter::rooms_owned() const noexcept {
    return impl_->rooms_owned();
}

std::size_t RoomRouter::rooms_joined() const noexcept {
    return impl_->rooms_joined();
}

const RegistryCounters& RoomRouter::registry_counters() const noexcept {
    return impl_->registry_counters();
}

const RouterCounters& RoomRouter::counters() const noexcept {
    return impl_->counters();
}

} // namespace rt
