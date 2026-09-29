#include "rt/room_router.hpp"

#include "net/slab.hpp"
#include "net/socket.hpp"

#include "node_auth.hpp"
#include "wire.hpp"

#include <algorithm>
#include <deque>
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

using RequestDone = std::move_only_function<void(wire::Status, std::uint64_t seq) noexcept>;

RouteError route_error(wire::Status status) noexcept {
    switch (status) {
    case wire::Status::Fenced:
        return RouteError::Fenced;
    case wire::Status::Busy:
        return RouteError::Busy;
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
                ++router_.counters_.slow_peers;
                close();
            }
        }

        [[nodiscard]] bool authenticated() const noexcept { return state_ == State::Authenticated; }
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
            if (hello == nullptr || hello->version != wire::kVersion) {
                router_.refused("no hello");
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
                router_.on_forwarded(handle_, f->request, f->room, f->sender, f->body);
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

        void request(std::uint64_t request, std::span<const std::byte> frame, RequestDone done) {
            pending_.push_back({.request = request,
                                .deadline = router_.clock_.now() + kForwardTimeout,
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
                    ++router_.counters_.slow_peers;
                    broken_ = true;
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
                p.done(wire::Status::Unavailable, 0);
            }
        }

        void close() noexcept {
            if (state_ == State::Connecting) {
                router_.reactor_.unwatch(connecting_.get());
                connecting_.reset();
            }
            if (state_ == State::Handshaking || state_ == State::Open) {
                router_.reactor_.begin_close(conn_);
            }
            state_ = State::Closed;
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
                done(f->status, f->seq);
                return true;
            }
            if (const auto* f = std::get_if<wire::Deliver>(&frame)) {
                router_.on_deliver(
                    Message{.room = f->room, .seq = f->seq, .sender = f->sender, .body = f->body});
                return true;
            }
            if (const auto* f = std::get_if<wire::Unsubscribe>(&frame)) {
                router_.disowned(f->room, node_);
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
        wire::Nonce own_nonce_{};
        State state_ = State::Locating;
        os::UniqueFd connecting_;
        net::ConnId conn_;
        bool opened_ = false;
        std::vector<std::byte> unsent_;
        std::vector<Pending> pending_;
        wire::Decoder decoder_;
    };

public:
    Impl(net::IReactor& reactor, IRoomStore& store, const core::ports::IClock& clock,
         core::ports::IRandom& random, RouterConfig config, IRouterEvents& events)
        : reactor_(reactor), store_(store), clock_(clock), random_(random),
          config_(std::move(config)), events_(events), incarnation_(core::Uuid::v7(clock, random)),
          registry_(store, clock, config_.self, incarnation_, *this), inbound_(kInboundSlots) {}

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
            done({});
            return;
        }
        if (lr.owner) {
            lr.members.push_back(&member);
            done({});
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
        drop_if_empty(room);
    }

    void send(const core::RoomId& room, IMember& from, const core::UserId& sender,
              std::vector<std::byte> body, SendCallback done) {
        const auto it = local_.find(room);
        if (it == local_.end() ||
            std::ranges::find(it->second.members, &from) == it->second.members.end()) {
            done(std::unexpected(RouteError::NotJoined));
            return;
        }
        // Owning comes first, whatever the cache says: a write under a generation that has
        // moved on is exactly what the fence must see and refuse.
        if (registry_.owned(room)) {
            enqueue(room, Write{.sender = sender,
                                .body = std::move(body),
                                .local = std::move(done),
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
        wire::encode_send(frame, request, room, sender, body);
        ++counters_.forwarded;
        link(owner->node)
            .request(request, frame,
                     [this, room, done = std::move(done)](wire::Status status,
                                                          std::uint64_t seq) mutable noexcept {
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
    };

    // A write waiting for its sequence number, from a member here or from another node.
    struct Write {
        core::UserId sender;
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
    };

    static std::size_t cost(const Write& write) noexcept {
        return write.body.size() + kWriteOverhead;
    }

    void tick() {
        const core::MonoTime now = clock_.now();
        inbound_.for_each_live([now](Inbound& in) {
            if (in.handshake_overdue(now)) {
                in.close();
            }
        });
        std::vector<Outbound*> stuck;
        for (auto& [node, link] : outbound_) {
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
            done(wire::Status::Unavailable, 0);
        }
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
                     [this, room, owner](wire::Status status, std::uint64_t) noexcept {
                         subscribed(room, owner, status);
                     });
    }

    void subscribed(const core::RoomId& room, const Ownership& owner, wire::Status status) {
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
        for (Joining& j : joined) {
            j.done({});
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
        const auto it = owned_.find(room);
        if (it == owned_.end() || it->second.appending || it->second.writes.empty()) {
            return;
        }
        it->second.appending = true;
        const bool started = registry_.append(
            room, [this, room](std::expected<std::uint64_t, AppendError> seq) noexcept {
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
            answer(write,
                   std::unexpected(seq.error() == AppendError::Fenced ? RouteError::Fenced
                                                                      : RouteError::Unavailable));
        } else {
            fan_out(room, o, *seq, write);
            answer(write, *seq);
        }
        pump(room);
    }

    void fan_out(const core::RoomId& room, OwnedRoomState& o, std::uint64_t seq,
                 const Write& write) {
        deliver_here(Message{.room = room, .seq = seq, .sender = write.sender, .body = write.body});
        if (o.subscribers.empty()) {
            return;
        }
        std::vector<std::byte> frame;
        wire::encode_deliver(frame, room, seq, write.sender, write.body);
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
        reply(peer, request, wire::Status::Ok, 0);
    }

    void on_unsubscribe(net::Slab<Inbound>::Handle peer, const core::RoomId& room) noexcept {
        const auto it = owned_.find(room);
        if (it != owned_.end()) {
            std::erase(it->second.subscribers, peer);
        }
    }

    void on_forwarded(net::Slab<Inbound>::Handle peer, std::uint64_t request,
                      const core::RoomId& room, const core::UserId& sender,
                      std::span<const std::byte> body) {
        Write write{.sender = sender,
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
    // seq check drops what a resubscription repeats.
    void on_deliver(const Message& message) { deliver_here(message); }

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
    std::unordered_map<core::RoomId, LocalRoom> local_;
    std::unordered_map<core::RoomId, OwnedRoomState> owned_;
    std::size_t queued_bytes_ = 0;
    // Owned rooms nobody here uses, and since when.
    std::unordered_map<core::RoomId, core::MonoTime> idle_since_;
    net::Slab<Inbound> inbound_;
    std::unordered_map<core::NodeId, std::unique_ptr<Outbound>> outbound_;
    std::vector<std::unique_ptr<Outbound>> closing_;
    std::uint64_t next_request_ = 1;
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
                      std::vector<std::byte> body, SendCallback done) {
    impl_->send(room, from, sender, std::move(body), std::move(done));
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
