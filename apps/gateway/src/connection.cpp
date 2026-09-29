#include "connection.hpp"

#include "core/util/hls.hpp"
#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/auth/token_extractor.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>

namespace gateway {

namespace {

using core::ports::CatalogError;
using core::ports::StorageError;
using http::Status;

// A create request is ~120 bytes of JSON; 4 KiB leaves room for a long filename and nothing
// else, so the body is buffered whole and parsed once.
constexpr std::size_t kMaxJsonBody = std::size_t{4} * 1024;
// Staging only ever holds what one parser callback handed over: one 64 KiB receive, or the
// parser's retained tail of at most 4 x 64 KiB.
constexpr std::size_t kMaxStaging = std::size_t{4} * 64 * 1024;
// Long enough for a client to read an error response before the socket goes away.
constexpr core::Millis kLinger{2'000};
constexpr std::size_t kResponseHead = 1024;
constexpr std::chrono::seconds kRetryAfter{5};

// Holds once the request is past authentication, lookup or claim, which is the only time the
// handlers below are reached; a miss is a bug, answered with 500 rather than undefined
// behaviour.
template <class T> [[nodiscard]] const T* get(const std::optional<T>& o) noexcept {
    return o ? &*o : nullptr;
}

std::string_view state_name(core::VideoState s) noexcept {
    switch (s) {
    case core::VideoState::Init:
        return "init";
    case core::VideoState::Uploading:
        return "uploading";
    case core::VideoState::Processing:
        return "processing";
    case core::VideoState::Ready:
        return "ready";
    case core::VideoState::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace

Connection::Connection(Handle handle, Gateway& gateway) : handle_(handle), gateway_(gateway) {}

Connection::~Connection() {
    release_slot();
}

void Connection::start(std::unique_ptr<net::ITransport> transport) noexcept {
    transport_ = std::move(transport);
    last_activity_ = now();
    receiving_ = true;
    transport_->start_receiving();
    arm_timer(gw().limits().header_timeout);
}

bool Connection::quiescent() const noexcept {
    // A connection retired before start() never reached the reactor.
    return pending_ == 0 && (!transport_ || transport_->is_quiescent());
}

void Connection::drain() noexcept {
    draining_ = true;
    if (phase_ == Phase::Idle) {
        close();
    }
}

void Connection::abort() noexcept {
    close();
}

void Connection::arm_timer(core::Millis delay) noexcept {
    deps().reactor.cancel_timer(timer_);
    timer_ = deps().reactor.arm_timer(delay, *this);
}

void Connection::restart_rate_window() noexcept {
    rate_window_start_ = now();
    rate_window_bytes_ = 0;
}

std::optional<core::Millis> Connection::check_body_rate(core::MonoTime t) noexcept {
    const Limits& limits = gw().limits();
    const auto span = std::chrono::duration_cast<core::Millis>(t - rate_window_start_);
    if (span < limits.body_rate_window) {
        return limits.body_rate_window - span;
    }
    // Over the time that actually passed, which a late timer makes longer than the window.
    const std::uint64_t floor =
        limits.min_body_bytes_per_second * static_cast<std::uint64_t>(span.count()) / 1000;
    if (rate_window_bytes_ < floor) {
        ++gw().counters().timeouts_body_rate;
        fail(Status::RequestTimeout);
        return std::nullopt;
    }
    restart_rate_window();
    return limits.body_rate_window;
}

// Every response carries the id, including one for a request that never parsed.
void Connection::begin_request() noexcept {
    phase_ = Phase::Request;
    ++request_seq_;
    request_started_ = now();
    core::Uuid::v7(deps().clock, deps().random).format_to(req_.request_id);
}

void Connection::on_data(net::BorrowedBytes bytes) noexcept {
    last_activity_ = now();
    if (phase_ == Phase::Lingering) {
        return;
    }
    if (phase_ == Phase::Idle) {
        begin_request();
    }
    on_parse(parser_.feed(bytes));
}

void Connection::on_parse(http::ParseResult result) noexcept {
    if (phase_ != Phase::Request && phase_ != Phase::Idle) {
        return;
    }
    if (!result) {
        if (phase_ == Phase::Idle) {
            // Pipelined bytes that fail before a head exists still get an answer and an id.
            begin_request();
        }
        if (!req_.responded) {
            const auto status = result.error().status;
            req_.keep_alive = req_.keep_alive && !result.error().must_close;
            if (!result.error().must_close) {
                req_.message_complete = true;
            }
            fail(status);
        }
        return;
    }
    if (req_.body_error) {
        fail(*req_.body_error);
        return;
    }
    switch (*result) {
    case http::ParseProgress::NeedMore:
        if (!receiving_ && !peer_eof_) {
            receiving_ = true;
            transport_->start_receiving();
        }
        break;
    case http::ParseProgress::Paused:
        parser_paused_ = true;
        if (receiving_) {
            receiving_ = false;
            transport_->stop_receiving();
        }
        break;
    case http::ParseProgress::MessageComplete:
        parser_paused_ = true;
        if (receiving_) {
            receiving_ = false;
            transport_->stop_receiving();
        }
        req_.message_complete = true;
        break;
    }
    advance();
}

http::HeadVerdict Connection::on_head(const http::RequestHead& head) noexcept {
    if (phase_ == Phase::Idle) {
        // A pipelined request, parsed from bytes that arrived with the previous one.
        begin_request();
    }
    ++gw().counters().requests;
    req_.content_length = head.content_length;
    req_.keep_alive =
        head.keep_alive && !draining_ && ++requests_ < gw().limits().max_requests_per_connection;

    const auto match = kRouter.match(head.method, head.target);
    if (!match) {
        req_.allow = match.error().allow;
        return http::HeadVerdict::reject(match.error().status);
    }
    req_.route = match->id;
    req_.params = match->params;
    if (head.content_length > 0) {
        last_progress_ = now();
        restart_rate_window();
        arm_timer(gw().limits().body_idle_timeout);
    }

    switch (match->id) {
    case RouteId::CreateUpload:
        if (head.content_length == 0) {
            return http::HeadVerdict::reject(Status::BadRequest);
        }
        if (head.content_length > kMaxJsonBody) {
            return http::HeadVerdict::reject(Status::ContentTooLarge);
        }
        break;
    case RouteId::AppendChunk: {
        const auto offset = http::find_header(head.headers, "upload-offset");
        req_.upload_offset = offset ? core::parse_integer<std::uint64_t>(*offset) : std::nullopt;
        if (!req_.upload_offset) {
            return http::HeadVerdict::reject(Status::BadRequest);
        }
        break;
    }
    case RouteId::MasterPlaylist:
    case RouteId::MediaPlaylist:
        ++(match->id == RouteId::MasterPlaylist ? gw().counters().playlists_master
                                                : gw().counters().playlists_media);
        [[fallthrough]];
    case RouteId::UploadOffset:
    case RouteId::CancelUpload:
    case RouteId::CommitUpload:
    case RouteId::GetVideo:
    case RouteId::Healthz:
    case RouteId::Readyz:
    case RouteId::Metrics:
        if (head.content_length != 0) {
            return http::HeadVerdict::reject(Status::BadRequest);
        }
        break;
    }

    if (!requires_auth(match->id)) {
        req_.authenticated = true;
    } else {
        infra::auth::TokenExtractor extractor(gw().limits().auth_cookie);
        for (const http::HeaderField& h : head.headers) {
            extractor.on_header(h.name, h.value);
        }
        const auto token = extractor.token();
        if (!token) {
            return http::HeadVerdict::reject(Status::Unauthorized);
        }
        req_.token = *token;
        authenticate();
        if (req_.body_error) {
            return http::HeadVerdict::reject(*req_.body_error);
        }
    }
    return http::HeadVerdict::accept();
}

void Connection::authenticate() noexcept {
    const auto result = deps().verifier.verify(req_.token, deps().clock.wall_now(), *this);
    if (!result) {
        key_wait_ = true;
        ++pending_;
        return;
    }
    if (!*result) {
        // An outage at the key server says nothing about the token; a 401 would sign out a
        // user whose session is fine, where a 503 asks them to retry.
        req_.body_error = result->error() == core::ports::AuthError::KeysUnavailable
                              ? Status::ServiceUnavailable
                              : Status::Unauthorized;
        return;
    }
    req_.claims = **result;
    req_.authenticated = true;
}

void Connection::on_keys_refreshed() noexcept {
    key_wait_ = false;
    --pending_;
    if (phase_ != Phase::Request) {
        return;
    }
    authenticate();
    if (req_.body_error) {
        fail(*req_.body_error);
        return;
    }
    advance();
}

http::BodyVerdict Connection::on_body(std::span<const std::byte> bytes) noexcept {
    last_activity_ = now();
    last_progress_ = last_activity_;
    if (req_.route == RouteId::CreateUpload) {
        std::ranges::transform(bytes, std::back_inserter(req_.body),
                               [](std::byte b) { return static_cast<char>(b); });
        return http::BodyVerdict::Continue;
    }
    gw().counters().bytes_ingested += bytes.size();
    rate_window_bytes_ += bytes.size();
    if (!session_ || staging_head_ < staging_.size()) {
        if (staging_.size() - staging_head_ + bytes.size() > kMaxStaging) {
            req_.body_error = Status::ContentTooLarge;
            return http::BodyVerdict::Pause;
        }
        staging_.insert(staging_.end(), bytes.begin(), bytes.end());
        return http::BodyVerdict::Pause;
    }
    const std::size_t taken = session_->write(bytes);
    if (taken < bytes.size()) {
        // Appended, never assigned: whatever is already staged stays in front.
        staging_.insert(staging_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(taken),
                        bytes.end());
        return http::BodyVerdict::Pause;
    }
    return http::BodyVerdict::Continue;
}

void Connection::on_message_complete() noexcept {}

void Connection::advance() noexcept {
    if (req_.responded || !req_.route || !req_.authenticated || phase_ != Phase::Request) {
        return;
    }
    switch (*req_.route) {
    case RouteId::Healthz:
        if (req_.message_complete) {
            respond({.status = Status::Ok, .content_type = "text/plain"}, "ok\n");
        }
        return;
    case RouteId::Readyz:
        if (req_.message_complete) {
            const bool ready = gw().ready();
            respond({.status = ready ? Status::Ok : Status::ServiceUnavailable,
                     .content_type = "text/plain"},
                    ready ? "ready\n" : "draining\n");
        }
        return;
    case RouteId::Metrics:
        if (req_.message_complete) {
            respond({.status = Status::Ok, .content_type = "text/plain; version=0.0.4"},
                    gw().render_metrics());
        }
        return;
    case RouteId::CreateUpload:
        if (req_.message_complete && !req_.started) {
            req_.started = true;
            start_create();
        }
        return;
    case RouteId::AppendChunk:
        if (!req_.started) {
            req_.started = true;
            start_append();
        } else if (req_.message_complete && session_ && !req_.finishing) {
            req_.finishing = true;
            session_->finish();
        }
        return;
    case RouteId::UploadOffset:
    case RouteId::CancelUpload:
    case RouteId::CommitUpload:
    case RouteId::GetVideo:
    case RouteId::MasterPlaylist:
    case RouteId::MediaPlaylist:
        if (req_.message_complete && !req_.started) {
            req_.started = true;
            start_lookup();
        }
        return;
    }
}

void Connection::start_create() noexcept {
    const auto doc = core::json::parse(req_.body);
    const core::json::Value* filename = doc ? doc->find("filename") : nullptr;
    const core::json::Value* size = doc ? doc->find("size_bytes") : nullptr;
    const core::json::Value* type = doc ? doc->find("content_type") : nullptr;
    const std::string_view title = filename != nullptr
                                       ? filename->as_string().value_or(std::string_view{})
                                       : std::string_view{};
    const std::uint64_t bytes = size != nullptr ? size->as_u64().value_or(0) : 0;
    const std::string_view type_text =
        type != nullptr ? type->as_string().value_or(std::string_view{}) : std::string_view{};
    auto content_type = core::ContentType::parse(type_text);
    // A missing or mistyped field reads as empty or zero, and every such value is refused.
    if (title.empty() || bytes == 0 || bytes > core::Upload::kMaxSizeBytes || !content_type ||
        !content_type->view().starts_with("video/")) {
        fail(Status::BadRequest);
        return;
    }
    const core::ports::Claims* claims = get(req_.claims);
    if (claims == nullptr) {
        fail(Status::InternalServerError);
        return;
    }
    const auto video = core::VideoId::generate(deps().clock, deps().random);
    // Checked before the store is asked for anything, so a refusal leaves nothing behind.
    const core::VideoRecord probe{.id = video,
                                  .owner = claims->subject,
                                  .title = std::string(title),
                                  .state = core::VideoState::Init,
                                  .version = 0,
                                  .error_reason = std::nullopt,
                                  .duration = std::nullopt};
    if (!core::Video::rehydrate(probe)) {
        fail(Status::BadRequest);
        return;
    }
    auto key = core::StorageKey::parse("videos/" + video.to_string() + "/raw");
    if (!key) {
        fail(Status::InternalServerError);
        return;
    }
    req_.create = PendingCreate{.video = video,
                                .upload = core::UploadId::generate(deps().clock, deps().random),
                                .title = std::string(title),
                                .size = bytes,
                                .type = std::move(*content_type),
                                .key = std::move(*key)};
    submit(ControlOp::Create);
}

void Connection::start_append() noexcept {
    const core::ports::Claims* claims = get(req_.claims);
    const auto id = core::UploadId::parse(req_.params[0]);
    if (claims == nullptr) {
        fail(Status::InternalServerError);
        return;
    }
    if (!id) {
        fail(Status::NotFound);
        return;
    }
    req_.upload_id = *id;
    switch (gw().acquire_upload_slot(claims->subject)) {
    case Admission::Admitted:
        slot_user_ = claims->subject;
        break;
    case Admission::UserAtLimit:
        ++gw().counters().admission_rejections;
        fail(Status::TooManyRequests);
        return;
    case Admission::Full:
        ++gw().counters().admission_rejections;
        fail(Status::ServiceUnavailable);
        return;
    }
    ++pending_;
    deps().catalog.claim_upload(*id, claims->subject, [this](auto result) noexcept {
        --pending_;
        on_claimed(std::move(result));
    });
}

void Connection::on_claimed(core::ports::CatalogResult<core::ports::StoredUpload> result) noexcept {
    if (result) {
        req_.claimed = true;
        req_.upload = std::move(*result);
    }
    if (phase_ != Phase::Request) {
        release_claim();
        return;
    }
    if (!result) {
        if (result.error() == CatalogError::Conflict) {
            // Someone else is appending; report where the upload stands so the client can
            // retry from there.
            start_lookup();
            return;
        }
        fail_catalog(result.error());
        return;
    }
    const core::ports::Claims* claims = get(req_.claims);
    const core::ports::StoredUpload* stored = get(req_.upload);
    const std::uint64_t* offset = get(req_.upload_offset);
    if (claims == nullptr || stored == nullptr || offset == nullptr) {
        release_claim();
        fail(Status::InternalServerError);
        return;
    }
    const core::UploadRecord& up = stored->upload;
    if (up.state != core::UploadState::Active) {
        release_claim();
        fail(Status::Conflict, up.durable_offset);
        return;
    }
    if (*offset != up.durable_offset) {
        // The catalog lags the store whenever a chunk became durable but its PATCH never
        // finished, and HEAD answers from the store. Ask the store before refusing.
        submit(ControlOp::Offset);
        return;
    }
    begin_append(up.durable_offset);
}

void Connection::begin_append(std::uint64_t at) noexcept {
    const core::ports::StoredUpload* stored = get(req_.upload);
    if (stored == nullptr) {
        release_claim();
        fail(Status::InternalServerError);
        return;
    }
    const core::UploadRecord& up = stored->upload;
    if (req_.content_length > up.size_bytes - at) {
        release_claim();
        fail(Status::BadRequest);
        return;
    }
    if (req_.content_length == 0) {
        release_claim();
        respond({.status = Status::NoContent, .upload_offset = at}, {});
        return;
    }
    auto session = deps().store.open(ingest_id(*stored), at, *this);
    if (!session) {
        release_claim();
        fail_storage(session.error());
        return;
    }
    session_ = std::move(*session);
    drain_staging();
}

core::ports::IngestId Connection::ingest_id(const core::ports::StoredUpload& s) {
    return core::ports::IngestId{.key = s.object_key,
                                 .backend_ref = s.backend_ref,
                                 .total_bytes = s.upload.size_bytes,
                                 .chunk_size = s.upload.chunk_size};
}

void Connection::drain_staging() noexcept {
    while (staging_head_ < staging_.size()) {
        const std::size_t n = session_->write(std::span(staging_).subspan(staging_head_));
        if (n == 0) {
            return;
        }
        staging_head_ += n;
        last_progress_ = now();
    }
    staging_.clear();
    staging_head_ = 0;
    if (parser_paused_ && !req_.message_complete) {
        parser_paused_ = false;
        // The time the store held the body up is not the client's to answer for.
        restart_rate_window();
        on_parse(parser_.resume());
    }
}

void Connection::on_ingest_progress() noexcept {
    if (!session_ || phase_ != Phase::Request) {
        return;
    }
    switch (session_->state()) {
    case core::ports::IngestState::Failed: {
        const StorageError error = session_->error().value_or(StorageError::Transient);
        session_.reset();
        release_claim();
        fail_storage(error);
        return;
    }
    case core::ports::IngestState::Committed:
        on_durable();
        return;
    case core::ports::IngestState::Open:
        drain_staging();
        advance();
        return;
    case core::ports::IngestState::Finalizing:
        return;
    }
}

void Connection::on_durable() noexcept {
    const std::uint64_t offset = session_->durable_offset();
    session_.reset();
    const core::UploadId* id = get(req_.upload_id);
    const core::ports::StoredUpload* stored = get(req_.upload);
    if (id == nullptr || stored == nullptr) {
        release_claim();
        fail(Status::InternalServerError);
        return;
    }
    ++pending_;
    deps().catalog.record_progress(
        *id, stored->upload.video_id, offset,
        [this, offset](core::ports::CatalogResult<void> result) noexcept {
            --pending_;
            release_claim();
            if (phase_ != Phase::Request) {
                return;
            }
            if (!result) {
                fail_catalog(result.error());
                return;
            }
            respond({.status = Status::NoContent, .upload_offset = offset}, {});
        });
}

void Connection::start_lookup() noexcept {
    if (req_.route == RouteId::GetVideo || req_.route == RouteId::MasterPlaylist ||
        req_.route == RouteId::MediaPlaylist) {
        const auto id = core::VideoId::parse(req_.params[0]);
        if (!id) {
            fail(Status::NotFound);
            return;
        }
        ++pending_;
        deps().catalog.find_video(*id, [this](auto result) noexcept {
            --pending_;
            on_video(std::move(result));
        });
        return;
    }
    if (!req_.upload_id) {
        const auto id = core::UploadId::parse(req_.params[0]);
        if (!id) {
            fail(Status::NotFound);
            return;
        }
        req_.upload_id = *id;
    }
    ++pending_;
    deps().catalog.find_upload(*req_.upload_id, [this](auto result) noexcept {
        --pending_;
        on_found(std::move(result));
    });
}

void Connection::on_found(core::ports::CatalogResult<core::ports::StoredUpload> result) noexcept {
    if (phase_ != Phase::Request) {
        return;
    }
    const core::ports::Claims* claims = get(req_.claims);
    if (claims == nullptr || !req_.route) {
        fail(Status::InternalServerError);
        return;
    }
    if (!result) {
        fail_catalog(result.error());
        return;
    }
    // Another user's upload is indistinguishable from a missing one.
    if (!(result->upload.owner == claims->subject)) {
        fail(Status::NotFound);
        return;
    }
    req_.upload = std::move(*result);
    const core::UploadRecord& up = req_.upload->upload;
    switch (*req_.route) {
    case RouteId::AppendChunk:
        // A concurrent append holds the upload.
        fail(Status::Conflict, up.durable_offset);
        return;
    case RouteId::UploadOffset:
        if (up.state == core::UploadState::Completed) {
            respond({.status = Status::NoContent, .upload_offset = up.size_bytes}, {});
            return;
        }
        submit(ControlOp::Offset);
        return;
    case RouteId::CommitUpload:
        if (up.state == core::UploadState::Aborted) {
            fail(Status::Conflict, up.durable_offset);
            return;
        }
        submit(ControlOp::Commit);
        return;
    case RouteId::CancelUpload:
        if (up.state == core::UploadState::Completed) {
            fail(Status::Conflict, up.size_bytes);
            return;
        }
        submit(ControlOp::Discard);
        return;
    case RouteId::CreateUpload:
    case RouteId::GetVideo:
    case RouteId::MasterPlaylist:
    case RouteId::MediaPlaylist:
    case RouteId::Healthz:
    case RouteId::Readyz:
    case RouteId::Metrics:
        fail(Status::InternalServerError);
        return;
    }
}

void Connection::on_video(core::ports::CatalogResult<core::VideoRecord> result) noexcept {
    if (phase_ != Phase::Request) {
        return;
    }
    const core::ports::Claims* claims = get(req_.claims);
    if (claims == nullptr) {
        fail(Status::InternalServerError);
        return;
    }
    if (!result) {
        fail_catalog(result.error());
        return;
    }
    if (!(result->owner == claims->subject)) {
        fail(Status::NotFound);
        return;
    }
    const core::VideoRecord& v = *result;
    if (req_.route != RouteId::GetVideo) {
        start_playlist(v);
        return;
    }
    std::string json = R"({"id":")" + v.id.to_string() + R"(","title":)";
    core::json::append_string(json, v.title);
    json +=
        std::format(R"(,"state":"{}","version":{},"duration_ms":)", state_name(v.state), v.version);
    json += v.duration ? std::to_string(v.duration->count()) : "null";
    json += "}";
    respond_json(Status::Ok, json);
}

// 409 for a video that exists but has nothing to play yet, or never will: the owner learns
// why, and a player that retries a 409 later gets the playlist once the worker is done. Only
// the owner gets this far, so it tells nobody else the id exists.
void Connection::start_playlist(const core::VideoRecord& video) noexcept {
    if (video.state != core::VideoState::Ready) {
        fail(Status::Conflict);
        return;
    }
    // A ready video always has one (the catalog enforces it); zero would still get the floor.
    const core::Millis duration = video.duration.value_or(core::Millis{0});
    ControlJob job;
    job.op = ControlOp::Playlist;
    job.playlist = PlaylistRequest{
        .kind = req_.route == RouteId::MasterPlaylist ? PlaylistKind::Master : PlaylistKind::Media,
        .video = video.id,
        .rendition = std::string(req_.params[1]),
        .ttl = core::hls::presign_ttl(duration)};
    start_job(std::move(job));
}

void Connection::on_playlist(ControlJob job) noexcept {
    const core::ports::Claims* claims = get(req_.claims);
    if (claims == nullptr || !job.body || !job.playlist) {
        fail(Status::InternalServerError);
        return;
    }
    if (!*job.body) {
        switch (job.body->error()) {
        case PlaylistFailure::NoSuchRendition:
            fail(Status::NotFound);
            return;
        case PlaylistFailure::Unavailable:
            fail(Status::ServiceUnavailable);
            return;
        case PlaylistFailure::Rejected:
            ++gw().counters().playlists_rejected;
            fail(Status::InternalServerError);
            return;
        case PlaylistFailure::Unsigned:
            ++gw().counters().presign_failures;
            fail(Status::InternalServerError);
            return;
        case PlaylistFailure::Broken:
            fail(Status::InternalServerError);
            return;
        }
        fail(Status::InternalServerError);
        return;
    }
    if (job.playlist->kind == PlaylistKind::Master) {
        gw().views().record({.video = job.playlist->video,
                             .viewer = claims->subject,
                             .at = deps().clock.wall_now()});
    }
    // Private: every URL in it is signed for this viewer. A minute lets the player's own cache
    // absorb its reloads without holding URLs close to expiry.
    respond({.status = Status::Ok,
             .content_type = "application/vnd.apple.mpegurl",
             .cache_control = "private, max-age=60"},
            **job.body);
}

void Connection::submit(ControlOp op) noexcept {
    ControlJob job;
    job.op = op;
    if (op == ControlOp::Create) {
        const PendingCreate* create = get(req_.create);
        if (create == nullptr) {
            fail(Status::InternalServerError);
            return;
        }
        job.key = create->key;
        job.size = create->size;
        job.type = create->type;
    } else {
        const core::ports::StoredUpload* stored = get(req_.upload);
        if (stored == nullptr) {
            fail(Status::InternalServerError);
            return;
        }
        job.ingest = ingest_id(*stored);
    }
    start_job(std::move(job));
}

void Connection::start_job(ControlJob job) noexcept {
    if (job_running_) {
        // job_ belongs to the pool thread until complete(); a request never outlives its job,
        // so reaching here is a bug, and overwriting job_ would be a data race.
        fail(Status::InternalServerError);
        return;
    }
    job.request = request_seq_;
    job_ = std::move(job);
    job_running_ = true;
    ++pending_;
    deps().pool.submit(*this);
}

// On the offload pool. Only job_ is touched here, and nothing else touches job_ until
// complete() runs.
void Connection::run() noexcept {
    core::ports::IIngestStore& store = deps().store;
    ControlJob& job = job_;
    const auto missing = std::unexpected(StorageError::Permanent);
    switch (job.op) {
    case ControlOp::Create:
        job.created = job.key && job.type ? store.create(*job.key, job.size, *job.type) : missing;
        return;
    case ControlOp::Offset:
        job.offset = job.ingest ? store.durable_offset(*job.ingest) : missing;
        return;
    case ControlOp::Commit:
        job.done = job.ingest ? store.commit(*job.ingest) : missing;
        return;
    case ControlOp::Discard:
        if (job.ingest) {
            store.discard(*job.ingest);
        }
        job.done = std::expected<void, StorageError>{};
        return;
    case ControlOp::Playlist:
        if (job.playlist) {
            job.body = build_playlist(deps().reader, *job.playlist, gw().limits().local_read_url);
        }
        return;
    case ControlOp::None:
        return;
    }
}

void Connection::complete() noexcept {
    --pending_;
    job_running_ = false;
    ControlJob job = std::exchange(job_, ControlJob{});
    if (phase_ != Phase::Request || job.request != request_seq_) {
        if (job.op == ControlOp::Create && job.created && *job.created) {
            // The request is gone; nothing will ever reference this ingest.
            gw().abandon(**job.created);
        }
        release_claim();
        return;
    }
    switch (job.op) {
    case ControlOp::Create:
        on_created(std::move(job));
        return;
    case ControlOp::Offset:
        on_offset(std::move(job));
        return;
    case ControlOp::Commit:
        on_committed(std::move(job));
        return;
    case ControlOp::Discard:
        if (req_.upload_id) {
            ++pending_;
            deps().catalog.abort_upload(*req_.upload_id,
                                        [this](core::ports::CatalogResult<void> result) noexcept {
                                            --pending_;
                                            if (phase_ != Phase::Request) {
                                                return;
                                            }
                                            if (!result) {
                                                fail_catalog(result.error());
                                                return;
                                            }
                                            respond({.status = Status::NoContent}, {});
                                        });
        }
        return;
    case ControlOp::Playlist:
        on_playlist(std::move(job));
        return;
    case ControlOp::None:
        return;
    }
}

void Connection::on_offset(ControlJob job) noexcept {
    if (!job.offset || !*job.offset) {
        release_claim();
        fail_storage(job.offset ? job.offset->error() : StorageError::Permanent);
        return;
    }
    const std::uint64_t durable = **job.offset;
    if (req_.route != RouteId::AppendChunk) {
        respond({.status = Status::NoContent, .upload_offset = durable}, {});
        return;
    }
    const std::uint64_t* offset = get(req_.upload_offset);
    const core::UploadId* id = get(req_.upload_id);
    const core::ports::StoredUpload* stored = get(req_.upload);
    if (offset == nullptr || id == nullptr || stored == nullptr) {
        release_claim();
        fail(Status::InternalServerError);
        return;
    }
    if (*offset != durable) {
        release_claim();
        fail(Status::Conflict, durable);
        return;
    }
    // The client resumes where the store says; bring the catalog up to it first, so that a
    // commit or a HEAD after this request agrees with both.
    ++pending_;
    deps().catalog.record_progress(
        *id, stored->upload.video_id, durable,
        [this, durable](core::ports::CatalogResult<void> result) noexcept {
            --pending_;
            if (phase_ != Phase::Request) {
                release_claim();
                return;
            }
            if (!result) {
                release_claim();
                fail_catalog(result.error());
                return;
            }
            begin_append(durable);
        });
}

void Connection::on_created(ControlJob job) noexcept {
    const core::ports::Claims* claims = get(req_.claims);
    const PendingCreate* create = get(req_.create);
    if (claims == nullptr || create == nullptr || !job.created) {
        fail(Status::InternalServerError);
        return;
    }
    if (!*job.created) {
        fail_storage(job.created->error());
        return;
    }
    const core::ports::IngestId& ingest = **job.created;
    core::ports::NewUpload rows{
        .video = core::VideoRecord{.id = create->video,
                                   .owner = claims->subject,
                                   .title = create->title,
                                   .state = core::VideoState::Init,
                                   .version = 0,
                                   .error_reason = std::nullopt,
                                   .duration = std::nullopt},
        .upload =
            core::UploadRecord{.id = create->upload,
                               .video_id = create->video,
                               .owner = claims->subject,
                               .size_bytes = create->size,
                               .chunk_size = ingest.chunk_size,
                               .durable_offset = 0,
                               .state = core::UploadState::Active,
                               .expires_at = deps().clock.wall_now() + gw().limits().upload_ttl},
        .backend_ref = ingest.backend_ref,
        .object_key = ingest.key,
    };
    if (!core::Video::rehydrate(rows.video) || !core::Upload::rehydrate(rows.upload)) {
        // start_create checked everything the client controls; the store's answer is at fault.
        gw().abandon(ingest);
        fail(Status::InternalServerError);
        return;
    }
    const std::uint64_t chunk = ingest.chunk_size;
    ++pending_;
    deps().catalog.create_upload(
        std::move(rows), [this, chunk, ingest](core::ports::CatalogResult<void> result) noexcept {
            --pending_;
            if (!result) {
                // Discarding is safe even if the insert did land: the row then points at an
                // ingest the upload reaper finds already gone, and discard is idempotent.
                gw().abandon(ingest);
            }
            const PendingCreate* created = get(req_.create);
            if (phase_ != Phase::Request || created == nullptr) {
                return;
            }
            if (!result) {
                fail_catalog(result.error());
                return;
            }
            respond_json(Status::Created,
                         std::format(R"({{"video_id":"{}","upload_id":"{}","chunk_size":{},)"
                                     R"("durable_offset":0}})",
                                     created->video.to_string(), created->upload.to_string(),
                                     chunk));
        });
}

void Connection::on_committed(ControlJob job) noexcept {
    const core::UploadId* id = get(req_.upload_id);
    const core::ports::StoredUpload* stored = get(req_.upload);
    if (id == nullptr || stored == nullptr || !job.done) {
        fail(Status::InternalServerError);
        return;
    }
    if (!*job.done) {
        if (job.done->error() == StorageError::PreconditionFailed) {
            fail(Status::Conflict, stored->upload.durable_offset);
            return;
        }
        fail_storage(job.done->error());
        return;
    }
    const core::VideoId video = stored->upload.video_id;
    ++pending_;
    deps().catalog.commit_upload(
        *id, video, std::string(request_id()),
        [this, video](core::ports::CatalogResult<void> result) noexcept {
            --pending_;
            if (phase_ != Phase::Request) {
                return;
            }
            if (!result) {
                fail_catalog(result.error());
                return;
            }
            respond_json(Status::Ok, std::format(R"({{"video_id":"{}","state":"processing"}})",
                                                 video.to_string()));
        });
}

void Connection::respond_json(Status status, std::string_view json) noexcept {
    respond({.status = status, .content_type = "application/json"}, json);
}

void Connection::fail(Status status, std::optional<std::uint64_t> upload_offset) noexcept {
    // A body the client is still sending cannot be skipped reliably; stop reading it and close.
    if (!req_.message_complete) {
        req_.keep_alive = false;
    }
    http::ResponseHead head{.status = status, .upload_offset = upload_offset};
    if (status == Status::MethodNotAllowed) {
        head.allow = req_.allow;
    }
    if (status == Status::ServiceUnavailable || status == Status::TooManyRequests) {
        head.retry_after = kRetryAfter;
    }
    respond(head, {});
}

void Connection::fail_storage(StorageError error) noexcept {
    switch (error) {
    case StorageError::NotFound:
        fail(Status::NotFound);
        return;
    case StorageError::PreconditionFailed:
    case StorageError::AlreadyExists:
        fail(Status::Conflict);
        return;
    case StorageError::Throttled:
    case StorageError::Transient:
        fail(Status::ServiceUnavailable);
        return;
    case StorageError::Unauthorized:
    case StorageError::Permanent:
    case StorageError::Corrupt:
        fail(Status::InternalServerError);
        return;
    }
}

void Connection::fail_catalog(CatalogError error) noexcept {
    switch (error) {
    case CatalogError::NotFound:
        fail(Status::NotFound);
        return;
    case CatalogError::Conflict:
        fail(Status::Conflict);
        return;
    case CatalogError::Unavailable:
        fail(Status::ServiceUnavailable);
        return;
    case CatalogError::Corrupt:
        fail(Status::InternalServerError);
        return;
    }
}

void Connection::respond(http::ResponseHead head, std::string_view body) noexcept {
    if (req_.responded || phase_ != Phase::Request) {
        return;
    }
    req_.responded = true;
    if (session_) {
        session_->abort();
        session_.reset();
    }
    release_slot();
    const bool keep = req_.keep_alive && req_.message_complete && !draining_;
    head.connection = keep ? http::Connection::KeepAlive : http::Connection::Close;
    head.request_id = request_id();
    head.content_length = body.size();
    std::array<char, kResponseHead> buf{};
    const auto n = http::write_response_head(head, buf);
    if (n) {
        transport_->send(std::as_bytes(std::span(buf.data(), *n)));
        if (!body.empty()) {
            transport_->send(std::as_bytes(std::span(body)));
        }
    } else {
        const std::string_view fallback =
            http::fixed_response(Status::InternalServerError, http::Connection::Close);
        transport_->send(std::as_bytes(std::span(fallback)));
        head.connection = http::Connection::Close;
    }
    if (head.connection == http::Connection::Close) {
        linger();
        return;
    }
    finish_request();
}

void Connection::finish_request() noexcept {
    req_ = Request{};
    staging_.clear();
    staging_head_ = 0;
    parser_paused_ = false;
    phase_ = Phase::Idle;
    last_activity_ = now();
    parser_.reset_for_next_request();
    if (draining_) {
        close();
        return;
    }
    // Parse whatever the client pipelined from the loop rather than from inside this response.
    resume_pending_ = true;
    arm_timer(core::Millis{0});
}

void Connection::release_slot() noexcept {
    if (slot_user_) {
        gateway_.release_upload_slot(*slot_user_);
        slot_user_.reset();
    }
}

void Connection::release_claim() noexcept {
    const core::UploadId* id = get(req_.upload_id);
    if (req_.claimed && id != nullptr) {
        deps().catalog.release_upload(*id);
    }
    req_.claimed = false;
}

void Connection::linger() noexcept {
    phase_ = Phase::Lingering;
    release_claim();
    // FIN after the response, then read and discard until the client closes or the linger
    // ends: closing with unread input would send RST and could destroy the response.
    transport_->shutdown_write();
    if (!receiving_ && !peer_eof_) {
        receiving_ = true;
        transport_->start_receiving();
    }
    arm_timer(kLinger);
}

void Connection::close() noexcept {
    if (phase_ == Phase::Closed) {
        return;
    }
    phase_ = Phase::Closed;
    if (session_) {
        session_->abort();
        session_.reset();
    }
    release_claim();
    release_slot();
    if (key_wait_) {
        deps().verifier.cancel_wait(*this);
        key_wait_ = false;
        --pending_;
    }
    deps().reactor.cancel_timer(timer_);
    timer_ = {};
    transport_->begin_close();
    gw().retire(handle_);
}

void Connection::on_writable() noexcept {}

void Connection::on_peer_eof() noexcept {
    peer_eof_ = true;
    receiving_ = false;
    if (phase_ == Phase::Request && req_.message_complete) {
        // Half-closed after a complete request: answer it, then close.
        req_.keep_alive = false;
        return;
    }
    close();
}

void Connection::on_error(int /*err*/) noexcept {
    close();
}

void Connection::on_timeout() noexcept {
    timer_ = {};
    if (phase_ == Phase::Closed) {
        return;
    }
    if (resume_pending_) {
        resume_pending_ = false;
        arm_timer(gw().limits().header_timeout);
        on_parse(parser_.resume());
        return;
    }
    if (phase_ == Phase::Lingering) {
        close();
        return;
    }
    const core::MonoTime t = now();
    const Limits& limits = gw().limits();
    if (phase_ == Phase::Idle) {
        const auto idle = std::chrono::duration_cast<core::Millis>(t - last_activity_);
        if (idle >= limits.header_timeout) {
            ++gw().counters().timeouts_header;
            close();
            return;
        }
        arm_timer(limits.header_timeout - idle);
        return;
    }
    const auto age = std::chrono::duration_cast<core::Millis>(t - request_started_);
    if (age >= limits.request_backstop) {
        ++gw().counters().timeouts_backstop;
        close();
        return;
    }
    if (!req_.route) {
        // Measured from the request's first byte, not the latest: a client dripping one byte
        // at a time must not hold a connection past the header timeout.
        if (age >= limits.header_timeout) {
            ++gw().counters().timeouts_header;
            close();
            return;
        }
        arm_timer(limits.header_timeout - age);
        return;
    }
    if (!req_.message_complete && req_.content_length > 0) {
        // Progress-based: a body is only stuck if nothing moved on either side of the pump.
        // Bytes waiting in staging mean the store stopped taking them, and a stalled store
        // also stops us reading the socket, so a client that left would go unnoticed.
        const auto idle = std::chrono::duration_cast<core::Millis>(t - last_progress_);
        if (idle >= limits.body_idle_timeout) {
            if (staging_head_ < staging_.size()) {
                ++gw().counters().timeouts_backend;
                fail(Status::ServiceUnavailable);
            } else {
                ++gw().counters().timeouts_body;
                fail(Status::RequestTimeout);
            }
            return;
        }
        core::Millis next = limits.body_idle_timeout - idle;
        // While the parser is paused the store is what holds the body up, and reading
        // restarts the window when it resumes.
        if (req_.route == RouteId::AppendChunk && !parser_paused_) {
            const auto window_left = check_body_rate(t);
            if (!window_left) {
                return;
            }
            next = std::min(next, *window_left);
        }
        arm_timer(next);
        return;
    }
    // Waiting on the catalog or the store: their own timeouts end the wait; this only keeps
    // the backstop in view.
    arm_timer(std::min(limits.request_backstop - age, limits.body_idle_timeout));
}

} // namespace gateway
