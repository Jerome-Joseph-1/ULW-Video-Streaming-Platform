#pragma once

#include "core/models/ids.hpp"
#include "core/ports/auth.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/storage.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"
#include "net/slab.hpp"
#include "net/transport.hpp"

#include "gateway.hpp"
#include "playback.hpp"
#include "routes.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gateway {

// One client connection and the request it is serving. Every step that waits (a key refresh,
// a catalog call, a blocking storage call on the offload pool, the object store taking bytes)
// comes back through one of the callbacks below, and each of them calls advance(), which
// looks at what is known and takes the next step. Nothing is sent from inside the parser's
// callbacks: they record what arrived, and on_parse() acts on it once the parser returns.
class Connection final : public net::IStreamHandler,
                         public net::ITimerHandler,
                         public http::IRequestSink,
                         public core::ports::IIngestObserver,
                         public core::ports::IKeyWaiter,
                         public net::IOffloadJob {
public:
    using Handle = net::Slab<Connection>::Handle;

    Connection(Handle handle, Gateway& gateway);
    ~Connection() override;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    void start(std::unique_ptr<net::ITransport> transport) noexcept;
    // The gateway is shutting down: finish the request in flight, then close.
    void drain() noexcept;
    // The drain deadline passed: close now, whatever is in flight.
    void abort() noexcept;
    // Nothing (the kernel, the catalog, the pool, the verifier) can still reach this object.
    [[nodiscard]] bool quiescent() const noexcept;
    // Heap bytes held for the request: staged body and a buffered JSON body.
    [[nodiscard]] std::size_t buffered_bytes() const noexcept {
        return staging_.capacity() + req_.body.capacity();
    }

    void on_data(net::BorrowedBytes bytes) noexcept override;
    void on_writable() noexcept override;
    void on_peer_eof() noexcept override;
    void on_error(int err) noexcept override;
    void on_timeout() noexcept override;

    [[nodiscard]] http::HeadVerdict on_head(const http::RequestHead& head) noexcept override;
    [[nodiscard]] http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override;
    void on_message_complete() noexcept override;

    void on_ingest_progress() noexcept override;
    void on_keys_refreshed() noexcept override;

    void run() noexcept override;
    void complete() noexcept override;

private:
    enum class Phase : std::uint8_t { Idle, Request, Lingering, Closed };
    enum class ControlOp : std::uint8_t { None, Create, Offset, Commit, Discard, Playlist };

    // Everything a create needs between parsing its body and the catalog insert.
    struct PendingCreate {
        core::VideoId video;
        core::UploadId upload;
        std::string title;
        std::uint64_t size = 0;
        core::ContentType type;
        core::StorageKey key;
    };

    // Fields ordered largest first so the struct packs without holes.
    struct Request {
        std::optional<core::ports::StoredUpload> upload;
        std::optional<PendingCreate> create;
        std::optional<core::ports::Claims> claims;
        std::string body;
        http::PathParams params{};
        std::string_view token;
        std::optional<std::uint64_t> upload_offset;
        std::optional<core::UploadId> upload_id;
        std::uint64_t content_length = 0;
        std::array<char, core::Uuid::kTextLength> request_id{};
        // Set when the store stopped taking the body, cleared when it took all that waited.
        std::optional<core::MonoTime> stalled_since;
        // When the chunk's first byte went to the store.
        core::MonoTime append_started;
        std::optional<http::Status> body_error;
        std::optional<RouteId> route;
        http::MethodSet allow;
        std::optional<http::Method> method;
        bool keep_alive = true;
        bool authenticated = false;
        bool message_complete = false;
        bool started = false;
        bool finishing = false;
        bool responded = false;
        bool claimed = false;
    };

    // One blocking storage job and its result. The inputs are copied in before the pool sees
    // the job, so nothing the pool thread reads can change under it, whatever happens to the
    // request meanwhile.
    struct ControlJob {
        ControlOp op = ControlOp::None;
        // The request that started the job; a completion for any other request is stale.
        std::uint64_t request = 0;
        std::optional<core::ports::IngestId> ingest;
        std::optional<core::StorageKey> key;
        std::uint64_t size = 0;
        std::optional<core::ContentType> type;
        std::optional<std::expected<core::ports::IngestId, core::ports::StorageError>> created;
        std::optional<std::expected<std::uint64_t, core::ports::StorageError>> offset;
        std::optional<std::expected<void, core::ports::StorageError>> done;
        std::optional<PlaylistRequest> playlist;
        std::optional<std::expected<std::string, PlaylistFailure>> body;
    };

    [[nodiscard]] Gateway& gw() const noexcept { return gateway_; }
    [[nodiscard]] const Deps& deps() const noexcept { return gateway_.deps(); }
    [[nodiscard]] std::string_view request_id() const noexcept {
        return {req_.request_id.data(), req_.request_id.size()};
    }

    void begin_request() noexcept;
    void on_parse(http::ParseResult result) noexcept;
    void advance() noexcept;
    void authenticate() noexcept;

    void start_create() noexcept;
    void start_append() noexcept;
    void start_lookup() noexcept;
    void on_claimed(core::ports::CatalogResult<core::ports::StoredUpload> result) noexcept;
    void on_found(core::ports::CatalogResult<core::ports::StoredUpload> result) noexcept;
    void on_video(core::ports::CatalogResult<core::VideoRecord> result) noexcept;
    void start_playlist(const core::VideoRecord& video) noexcept;
    void on_playlist(ControlJob job) noexcept;
    void on_durable() noexcept;
    void drain_staging() noexcept;
    void submit(ControlOp op) noexcept;
    void start_job(ControlJob job) noexcept;
    void on_created(ControlJob job) noexcept;
    void on_committed(ControlJob job) noexcept;
    [[nodiscard]] static core::ports::IngestId ingest_id(const core::ports::StoredUpload& s);
    void on_offset(ControlJob job) noexcept;
    void begin_append(std::uint64_t at) noexcept;

    void respond(http::ResponseHead head, std::string_view body) noexcept;
    void record_response(http::Status status) noexcept;
    [[nodiscard]] std::string_view readiness_body() const noexcept;
    void respond_json(http::Status status, std::string_view json) noexcept;
    void fail(http::Status status,
              std::optional<std::uint64_t> upload_offset = std::nullopt) noexcept;
    void fail_storage(core::ports::StorageError error) noexcept;
    void fail_catalog(core::ports::CatalogError error) noexcept;
    void finish_request() noexcept;
    void release_claim() noexcept;
    void release_slot() noexcept;
    void linger() noexcept;
    void close() noexcept;
    void arm_timer(core::Millis delay) noexcept;
    void restart_rate_window() noexcept;
    // Ends a chunk body that fell below the minimum rate, or returns how long until the
    // current window closes.
    [[nodiscard]] std::optional<core::Millis> check_body_rate(core::MonoTime t) noexcept;
    [[nodiscard]] core::MonoTime now() const noexcept { return deps().reactor.now(); }

    Handle handle_;
    Gateway& gateway_;
    // Set by start(); plaintext either way, whatever the socket carries.
    std::unique_ptr<net::ITransport> transport_;
    http::RequestParser parser_{*this};
    Phase phase_ = Phase::Idle;
    Request req_;
    net::TimerId timer_;
    core::MonoTime last_activity_;
    // Bytes moved in either direction of the body pump: from the client or into the store.
    core::MonoTime last_progress_;
    core::MonoTime request_started_;
    // The stretch of a chunk body over which the minimum rate is judged, and the client bytes
    // it has brought. Restarted whenever reading resumes after the store held the body up.
    core::MonoTime rate_window_start_;
    std::uint64_t rate_window_bytes_ = 0;
    std::size_t requests_ = 0;
    // Numbers each request on this connection, so late completions can tell whose they are.
    std::uint64_t request_seq_ = 0;
    bool receiving_ = false;
    bool parser_paused_ = false;
    bool resume_pending_ = false;
    bool draining_ = false;
    bool peer_eof_ = false;
    // Outstanding catalog callbacks, offload jobs and key waits: all hold `this`.
    int pending_ = 0;
    bool key_wait_ = false;

    // The upload slot the current PATCH holds, released when that request ends.
    std::optional<core::UserId> slot_user_;

    // Body bytes the store has not taken yet. Only filled while the parser is paused, so it
    // never holds more than one receive buffer or the parser's retained tail.
    std::vector<std::byte> staging_;
    std::size_t staging_head_ = 0;
    std::unique_ptr<core::ports::IIngestSession> session_;

    // Only the pool thread touches this between submit and complete().
    ControlJob job_;
    bool job_running_ = false;
};

} // namespace gateway
