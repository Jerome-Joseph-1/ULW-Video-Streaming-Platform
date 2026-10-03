#include "connection.hpp"

#include "core/util/hls.hpp"
#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "http/ascii.hpp"
#include "http/origin.hpp"
#include "infra/auth/token_extractor.hpp"
#include "net/socket.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <format>
#include <iterator>
#include <utility>

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
// How often a connection that stopped reading behind a held-back response looks whether the
// client has taken it: a request the client sends once it has waits at most this long.
constexpr core::Millis kDrainCheck{100};
constexpr std::size_t kResponseHead = 1024;
constexpr std::chrono::seconds kRetryAfter{5};

void count_playlist(Counters& counters, RouteId route) noexcept {
    if (route == RouteId::MasterPlaylist) {
        ++counters.playlists_master;
    } else if (route == RouteId::MediaPlaylist) {
        ++counters.playlists_media;
    } else {
        ++counters.playlists_live;
    }
}

// Holds once the request is past authentication, lookup or claim, which is the only time the
// handlers below are reached; a miss is a bug, answered with 500 rather than undefined
// behaviour.
template <class T> [[nodiscard]] const T* get(const std::optional<T>& o) noexcept {
    return o ? &*o : nullptr;
}

std::string_view method_name(http::Method m) noexcept {
    switch (m) {
    case http::Method::Get:
        return "GET";
    case http::Method::Head:
        return "HEAD";
    case http::Method::Post:
        return "POST";
    case http::Method::Put:
        return "PUT";
    case http::Method::Patch:
        return "PATCH";
    case http::Method::Delete:
        return "DELETE";
    case http::Method::Options:
        return "OPTIONS";
    case http::Method::Other:
        return "OTHER";
    }
    return "OTHER";
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

// A cross-site page can make a browser send a POST that carries the auth cookie, with a body of
// its choosing, only under a CORS-safelisted Content-Type (text/plain, a form's two types);
// application/json needs a preflight, which this gateway never answers.
[[nodiscard]] bool declares_json(std::span<const http::HeaderField> headers) noexcept {
    const auto type = http::find_header(headers, "content-type");
    if (!type) {
        return false;
    }
    std::string_view media = type->substr(0, type->find(';'));
    while (!media.empty() && (media.back() == ' ' || media.back() == '\t')) {
        media.remove_suffix(1);
    }
    return http::iequals(media, "application/json");
}

// A browser attaches the cookie to requests any page makes: an <img> or a no-cors fetch from
// another site reaches the gateway with the user's token. Whether the page is one the gateway
// trusts it learns from two headers no page can set (Fetch "forbidden" names):
//   Sec-Fetch-Site  where the page stands. Present, it must be same-origin, none (the user typed
//                   the URL) or, if the deployment says so, same-site. Browsers before 2023 omit
//                   it.
//   Origin          the page itself. Every browser sends it on a POST, PATCH or DELETE, same-origin
//                   ones included, and on a cross-origin GET; not on a same-origin GET, nor from
//                   <video> or <img>. When sent it must be allowed; a method that changes anything
//                   must send it.
[[nodiscard]] bool cookie_request_trusted(const http::RequestHead& head,
                                          std::optional<RouteId> route,
                                          const Limits& limits) noexcept {
    if (const auto site = http::find_header(head.headers, "sec-fetch-site")) {
        const bool trusted = *site == "same-origin" || *site == "none" ||
                             (limits.allow_same_site && *site == "same-site");
        if (!trusted) {
            return false;
        }
    }
    const auto origin = http::find_header(head.headers, "origin");
    if (origin && !http::origin_allowed(limits.allowed_origins, *origin)) {
        return false;
    }
    const bool safe = head.method == http::Method::Get || head.method == http::Method::Head ||
                      head.method == http::Method::Options;
    if (!safe && !origin) {
        return false;
    }
    // A second layer for the one route that reads a body as JSON: even from an allowed page,
    // a body is believed only under the type no other page can send without a preflight.
    return route != RouteId::CreateUpload || declares_json(head.headers);
}

// An upload past its expires_at that was never committed takes nothing more: 410, as tus's
// Expiration extension answers. The upload reaper aborts it later (ADR 0049), and until it does
// the rows and the stored bytes are still there, so without this a PATCH or commit would go
// through. A cancelled one answers the same, so the answer does not depend on whether the
// reaper has run yet; a committed one keeps the answers it had. The same test as
// Upload::is_expired.
[[nodiscard]] bool expired(const core::UploadRecord& up, core::WallTime now) noexcept {
    return up.state != core::UploadState::Completed && now >= up.expires_at;
}

} // namespace

Connection::Connection(Handle handle, Gateway& gateway)
    : handle_(handle), gateway_(gateway),
      claim_(gateway.deps().catalog, gateway.claims_held_count()) {}

Connection::~Connection() {
    release_slot();
    release_client_holds();
}

void Connection::start(std::unique_ptr<net::ITransport> transport, int fd,
                       const net::IpAddress& peer, std::optional<ClientHold> hold) noexcept {
    transport_ = std::move(transport);
    fd_ = fd;
    peer_ = peer;
    connection_hold_ = hold;
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
    if (phase_ != Phase::Idle) {
        return;
    }
    // A client still reading its last response is given the linger to finish it, as after a
    // response that closes; one with nothing waiting is closed at once.
    if (output_waiting()) {
        linger();
        return;
    }
    close();
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
    // The last request's claim went when it finished.
    assert(!claim_.held() && "a request began while a claim was held");
    phase_ = Phase::Request;
    ++request_seq_;
    request_started_ = now();
    core::Uuid::v7(deps().clock, deps().random).format_to(req_.request_id);
}

void Connection::on_data(net::BorrowedBytes bytes) noexcept {
    if (phase_ == Phase::Lingering) {
        last_activity_ = now();
        return;
    }
    // The first bytes of a new request, while the last response is held back: kept until it
    // has gone, at most the one receive that brought them, since reading stops here.
    if (phase_ == Phase::Idle && (awaiting_drain_ || hold_back_request())) {
        held_.insert(held_.end(), bytes.begin(), bytes.end());
        return;
    }
    parse(bytes);
}

void Connection::parse(net::BorrowedBytes bytes) noexcept {
    last_activity_ = now();
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
        if (!parser_paused_) {
            parser_paused_at_ = now();
        }
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
    if (!admit_forwarded(head)) {
        return http::HeadVerdict::reject(Status::TooManyRequests);
    }
    req_.method = head.method;
    req_.content_length = head.content_length;
    // Counted whether or not the connection stays open: one that closes after this response
    // never reads the count again.
    ++requests_;
    req_.keep_alive =
        head.keep_alive && !draining_ && requests_ < gw().limits().max_requests_per_connection;

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
    case RouteId::LivePlaylist:
        count_playlist(gw().counters(), match->id);
        [[fallthrough]];
    // The stream service's requests carry nothing: the token says who asks, the path which
    // stream.
    case RouteId::CreateStream:
    case RouteId::StreamStatus:
    case RouteId::StreamTicket:
    case RouteId::StartStream:
    case RouteId::EndStream:
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
        return http::HeadVerdict::accept();
    }
    return authenticate_head(head);
}

http::HeadVerdict Connection::authenticate_head(const http::RequestHead& head) noexcept {
    infra::auth::TokenExtractor extractor(gw().limits().auth_cookie);
    for (const http::HeaderField& h : head.headers) {
        extractor.on_header(h.name, h.value);
    }
    const auto token = extractor.token();
    if (!token) {
        return http::HeadVerdict::reject(Status::Unauthorized);
    }
    // Authorization wins over the cookie, so without one the token is the cookie. Checked before
    // the token is verified: a request some other page made is refused whatever it carries, and
    // charges none of the user's quota.
    if (!http::find_header(head.headers, "authorization") &&
        !cookie_request_trusted(head, req_.route, gw().limits())) {
        ++gw().counters().cross_site_rejections;
        return http::HeadVerdict::reject(Status::Forbidden);
    }
    req_.token = *token;
    authenticate();
    if (req_.body_error) {
        return http::HeadVerdict::reject(*req_.body_error);
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
    if (const auto charged = gw().charge_request(req_.claims->subject); !charged) {
        ++gw().counters().limited_user_requests;
        req_.retry_after = retry_after(charged.error());
        req_.body_error = Status::TooManyRequests;
        return;
    }
    req_.authenticated = true;
    // From here the user's own limits govern the request. One address can carry hundreds of
    // users (a carrier-grade NAT), and holding them all to 20 in flight would let the busiest
    // few lock the rest out; the address count is for the requests nobody vouches for.
    release_request_hold();
}

bool Connection::admit_forwarded(const http::RequestHead& head) noexcept {
    if (connection_hold_) {
        return true;
    }
    const net::IpAddress client =
        forwarded_client(peer_, head.headers, gw().limits().trusted_proxy_hops);
    if (deps().log.enabled(ops::Level::Debug)) {
        net::IpAddress::Text text{};
        deps().log.debug("forwarded client",
                         {{"request_id", request_id()}, {"client", client.format(text)}});
    }
    request_hold_ = gw().hold_client(client);
    if (request_hold_) {
        return true;
    }
    ++gw().counters().limited_ip_requests;
    // The hold frees as soon as one of the client's requests ends.
    req_.retry_after = std::chrono::seconds{1};
    return false;
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
    req_.bytes_received += bytes.size();
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
        if (!req_.stalled_since) {
            req_.stalled_since = last_activity_;
        }
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
            const std::string_view body = readiness_body();
            respond({.status = body == "ready\n" ? Status::Ok : Status::ServiceUnavailable,
                     .content_type = "text/plain"},
                    body);
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
    case RouteId::LivePlaylist:
    case RouteId::CreateStream:
    case RouteId::StreamStatus:
    case RouteId::StreamTicket:
    case RouteId::StartStream:
    case RouteId::EndStream:
        if (req_.message_complete && !req_.started) {
            req_.started = true;
            start_bodiless();
        }
        return;
    }
}

// The requests that carry no body start once their head is in.
void Connection::start_bodiless() noexcept {
    if (req_.route == RouteId::LivePlaylist) {
        start_live();
    } else if (req_.route == RouteId::CreateStream || req_.route == RouteId::StreamStatus ||
               req_.route == RouteId::StreamTicket || req_.route == RouteId::StartStream ||
               req_.route == RouteId::EndStream) {
        start_stream_route();
    } else {
        start_lookup();
    }
}

// 503 while starting, draining, or cut off from the database or the store: a load balancer
// should send this replica nothing it would have to refuse.
std::string_view Connection::readiness_body() const noexcept {
    if (gw().draining()) {
        return "draining\n";
    }
    switch (gw().deps().health.readiness(now())) {
    case Readiness::Ready:
        return "ready\n";
    case Readiness::Starting:
        return "starting\n";
    case Readiness::DatabaseDown:
        return "database unreachable\n";
    case Readiness::StoreDown:
        return "object store unreachable\n";
    case Readiness::Stale:
        return "health probe stuck\n";
    }
    return "starting\n";
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
    // Charged by what the PATCH says it carries, before any of it is read: the bytes a
    // refusal would otherwise have let in are what the quota exists to keep out.
    if (const auto charged = gw().charge_upload_bytes(claims->subject, req_.content_length);
        !charged) {
        ++gw().counters().limited_user_bytes;
        req_.retry_after = retry_after(charged.error());
        fail(Status::TooManyRequests);
        return;
    }
    req_.bytes_charged = req_.content_length;
    ++pending_;
    deps().catalog.claim_upload(*id, claims->subject,
                                [this, request = request_seq_, upload = *id](auto result) noexcept {
                                    --pending_;
                                    on_claimed(request, upload, std::move(result));
                                });
}

void Connection::on_claimed(
    std::uint64_t request, const core::UploadId& upload,
    core::ports::CatalogResult<core::ports::ClaimedUpload> result) noexcept {
    if (!serving(request)) {
        // The request ended while the claim was being taken. A claim granted to it goes
        // straight back, and nothing a later request holds is touched.
        if (result) {
            deps().catalog.release_upload(upload, result->token);
        }
        return;
    }
    if (result) {
        claim_.adopt(request, upload, result->token);
        req_.upload = std::move(result->stored);
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
        fail(Status::InternalServerError);
        return;
    }
    const core::UploadRecord& up = stored->upload;
    if (expired(up, deps().clock.wall_now())) {
        fail(Status::Gone);
        return;
    }
    if (up.state != core::UploadState::Active) {
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
        fail(Status::InternalServerError);
        return;
    }
    const core::UploadRecord& up = stored->upload;
    if (req_.content_length > up.size_bytes - at) {
        fail(Status::BadRequest);
        return;
    }
    if (req_.content_length == 0) {
        respond({.status = Status::NoContent, .upload_offset = at}, {});
        return;
    }
    auto session = deps().store.open(ingest_id(*stored), at, *this);
    if (!session) {
        fail_storage(session.error());
        return;
    }
    session_ = std::move(*session);
    req_.append_started = now();
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
            if (!req_.stalled_since) {
                req_.stalled_since = now();
            }
            return;
        }
        staging_head_ += n;
        last_progress_ = now();
    }
    staging_.clear();
    staging_head_ = 0;
    end_stall();
    if (parser_paused_ && !req_.message_complete) {
        parser_paused_ = false;
        // The time the store held the body up is not the client's to answer for, but the
        // bytes the client sent before it are: the window skips the hold and keeps them.
        // Restarting it here instead dropped them, and a client that sent fast, was held for
        // a moment, then slowed to a legal rate was judged on its slow part alone.
        rate_window_start_ += now() - parser_paused_at_;
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
    gw().part_upload_duration().observe(
        std::chrono::duration_cast<core::Millis>(now() - req_.append_started));
    const std::uint64_t offset = session_->durable_offset();
    session_.reset();
    const core::UploadId* id = get(req_.upload_id);
    const core::ports::StoredUpload* stored = get(req_.upload);
    // Recorded under this request's own grant: one that has been lost records nothing.
    const auto token = claim_.token_of(request_seq_);
    if (id == nullptr || stored == nullptr || !token) {
        fail(Status::InternalServerError);
        return;
    }
    ++pending_;
    deps().catalog.record_progress(
        *id, *token, stored->upload.video_id, offset,
        [this, offset, request = request_seq_](core::ports::CatalogResult<void> result) noexcept {
            --pending_;
            if (!serving(request)) {
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
    if (expired(up, deps().clock.wall_now()) &&
        (req_.route == RouteId::AppendChunk || req_.route == RouteId::UploadOffset ||
         req_.route == RouteId::CommitUpload)) {
        fail(Status::Gone);
        return;
    }
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
    case RouteId::LivePlaylist:
    case RouteId::CreateStream:
    case RouteId::StreamStatus:
    case RouteId::StreamTicket:
    case RouteId::StartStream:
    case RouteId::EndStream:
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
    // Only a failed video has one, and it is written for its owner (the worker's public_reason,
    // the reaper's "upload expired"); the details stay in the logs.
    if (v.state == core::VideoState::Failed && v.error_reason) {
        json += R"(,"error_reason":)";
        core::json::append_string(json, *v.error_reason);
    }
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
        fail_playlist(job.body->error());
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

void Connection::fail_playlist(PlaylistFailure failure) noexcept {
    switch (failure) {
    case PlaylistFailure::NoSuchRendition:
    case PlaylistFailure::Absent:
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
}

// Any signed-in viewer may watch any stream: a live stream is a broadcast, and nothing on the
// platform records who may see one (ADR-0059). An id the packager could never have used is
// answered like a stream that has not started.
void Connection::start_live() noexcept {
    const std::string_view stream = req_.params[0];
    if (!valid_stream_id(stream)) {
        fail(Status::NotFound);
        return;
    }
    ++pending_;
    gw().live().get(stream, *this);
}

void Connection::on_live_playlist(
    const std::expected<LiveAnswer, PlaylistFailure>& answer) noexcept {
    --pending_;
    // A waiting request ends only with its connection (the backstop and the drain deadline
    // close it), so an answer that finds no request in progress is for one that is gone.
    if (phase_ != Phase::Request) {
        return;
    }
    if (req_.route == RouteId::StreamStatus) {
        const core::ports::LiveStream* stream = get(req_.stream);
        if (stream == nullptr) {
            fail(Status::InternalServerError);
            return;
        }
        // The packager ends the playlist the moment the stream ends; the row follows it here,
        // or at the next sweep. A playlist not there yet, or unreadable, says nothing.
        if (answer && answer->ended && deps().live_streams != nullptr) {
            deps().live_streams->playlist_ended(stream->id);
            core::ports::LiveStream ended = *stream;
            ended.state = core::ports::LiveState::Ended;
            ended.ended_at = deps().clock.wall_now();
            ended.ended_by = core::ports::LiveEnd::Finished;
            respond_stream(Status::Ok, ended, nullptr);
            return;
        }
        respond_stream(Status::Ok, *stream, nullptr);
        return;
    }
    if (!answer) {
        fail_playlist(answer.error());
        return;
    }
    // Private: the URLs are signed. A live playlist is never reused from the viewer's own
    // cache: the gateway's copy already lags the store by up to half a segment, and a second
    // layer of age would push the player further behind the live edge. An ended one is final
    // and is kept like a VOD playlist.
    respond({.status = Status::Ok,
             .content_type = "application/vnd.apple.mpegurl",
             .cache_control = answer->ended ? "private, max-age=60" : "private, no-cache"},
            answer->body);
}

namespace {

std::int64_t unix_seconds(core::WallTime t) noexcept {
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count();
}

void append_time(std::string& json, const std::optional<core::WallTime>& t) {
    json += t ? std::to_string(unix_seconds(*t)) : "null";
}

void append_ticket(std::string& json, const core::ports::MediaTicket& ticket) {
    json += R"({"url":)";
    core::json::append_string(json, ticket.endpoint);
    json += R"(,"token":)";
    core::json::append_string(json, ticket.credential);
    json += R"(,"expires_at":)";
    json += std::to_string(unix_seconds(ticket.expires_at));
    json += '}';
}

} // namespace

// The stream id in the path is a stream service id (a UUID); anything else is no such stream.
// The owner of each action is the token's user: only they may publish, go live or end it.
void Connection::start_stream_route() noexcept {
    LiveStreams* live = deps().live_streams;
    const core::ports::Claims* claims = get(req_.claims);
    if (live == nullptr || claims == nullptr || !req_.route) {
        fail(live == nullptr ? Status::NotFound : Status::InternalServerError);
        return;
    }
    if (req_.route == RouteId::CreateStream) {
        // A deployment may keep broadcasting to the users a claim names
        // (ULW_LIVE_BROADCASTER_CLAIM); everything else stays open to any signed-in user.
        if (!claims->may_broadcast) {
            fail(Status::Forbidden);
            return;
        }
        create_stream(*live, claims->subject);
        return;
    }
    const auto id = core::LiveStreamId::parse(req_.params[0]);
    if (!id) {
        fail(Status::NotFound);
        return;
    }
    act_on_stream(*live, *id, claims->subject);
}

void Connection::create_stream(LiveStreams& live, const core::UserId& user) noexcept {
    ++pending_;
    live.create(user, [this](std::expected<StartedStream, LiveFailure> r) noexcept {
        --pending_;
        if (phase_ != Phase::Request) {
            return;
        }
        if (!r) {
            fail_live(r.error());
            return;
        }
        respond_stream(r->created ? Status::Created : Status::Ok, r->stream, &r->ticket);
    });
}

void Connection::act_on_stream(LiveStreams& live, const core::LiveStreamId& id,
                               const core::UserId& user) noexcept {
    const auto answer_stream =
        [this](std::expected<core::ports::LiveStream, LiveFailure> r) noexcept {
            --pending_;
            if (phase_ != Phase::Request) {
                return;
            }
            if (!r) {
                fail_live(r.error());
                return;
            }
            if (req_.route == RouteId::StreamStatus) {
                on_stream_status(*r);
                return;
            }
            respond_stream(Status::Ok, *r, nullptr);
        };
    ++pending_;
    if (req_.route == RouteId::StreamStatus) {
        live.status(id, answer_stream);
    } else if (req_.route == RouteId::StartStream) {
        live.go_live(id, user, answer_stream);
    } else if (req_.route == RouteId::EndStream) {
        live.end(id, user, answer_stream);
    } else {
        live.ticket(id, user,
                    [this](std::expected<core::ports::MediaTicket, LiveFailure> r) noexcept {
                        --pending_;
                        if (phase_ != Phase::Request) {
                            return;
                        }
                        if (!r) {
                            fail_live(r.error());
                            return;
                        }
                        std::string json;
                        append_ticket(json, *r);
                        // A credential: no cache may keep it.
                        respond({.status = Status::Ok,
                                 .content_type = "application/json",
                                 .cache_control = "no-store"},
                                json);
                    });
    }
}

// A live stream's row can lag its end by a sweep: the playlist, which the packager ends at
// once, is asked too, through the cache every viewer's player already reads.
void Connection::on_stream_status(const core::ports::LiveStream& stream) noexcept {
    if (stream.state != core::ports::LiveState::Live) {
        respond_stream(Status::Ok, stream, nullptr);
        return;
    }
    req_.stream = stream;
    ++pending_;
    gw().live().get(stream.id.to_string(), *this);
}

void Connection::respond_stream(http::Status status, const core::ports::LiveStream& stream,
                                const core::ports::MediaTicket* ticket) noexcept {
    const core::ports::Claims* claims = get(req_.claims);
    const bool owner = claims != nullptr && claims->subject == stream.owner;
    const std::string id = stream.id.to_string();
    std::string json = R"({"id":")" + id + R"(","state":")";
    json += core::ports::to_string(stream.state);
    json += R"(","playlist":"/api/v1/live/)" + id + R"(/index.m3u8","created_at":)";
    append_time(json, stream.created_at);
    json += R"(,"live_at":)";
    append_time(json, stream.live_at);
    json += R"(,"ended_at":)";
    append_time(json, stream.ended_at);
    json += R"(,"ended_by":)";
    if (stream.ended_by) {
        core::json::append_string(json, core::ports::to_string(*stream.ended_by));
    } else {
        json += "null";
    }
    // The recording is the owner's video like any upload (live.md); nobody else learns of it.
    if (owner) {
        json += R"(,"video_id":)";
        if (stream.recording) {
            core::json::append_string(json, stream.recording->to_string());
        } else {
            json += "null";
        }
    }
    if (ticket != nullptr) {
        json += R"(,"publish":)";
        append_ticket(json, *ticket);
    }
    json += '}';
    // The owner's view and a ticket are the owner's alone; a status is stale in seconds.
    respond({.status = status, .content_type = "application/json", .cache_control = "no-store"},
            json);
}

void Connection::fail_live(LiveFailure failure) noexcept {
    switch (failure) {
    case LiveFailure::NotFound:
        fail(Status::NotFound);
        return;
    case LiveFailure::Ended:
        fail(Status::Conflict);
        return;
    case LiveFailure::Full:
        // Streams end; a minute is about how soon one might.
        req_.retry_after = std::chrono::seconds{60};
        fail(Status::ServiceUnavailable);
        return;
    case LiveFailure::RateLimited:
        // The hour the count looks back over moves on; a stream's worth of it is a few minutes.
        req_.retry_after = std::chrono::seconds{600};
        fail(Status::TooManyRequests);
        return;
    case LiveFailure::Unavailable:
        req_.retry_after = std::chrono::seconds{2};
        fail(Status::ServiceUnavailable);
        return;
    case LiveFailure::Internal:
        fail(Status::InternalServerError);
        return;
    }
    fail(Status::InternalServerError);
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
    if (!serving(job.request)) {
        if (job.op == ControlOp::Create && job.created && *job.created) {
            // The request is gone; nothing will ever reference this ingest.
            gw().abandon(**job.created);
        }
        // The request's claim went when it ended. Whatever the connection serves now holds a
        // claim of its own, which this completion has no part in.
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
            deps().catalog.abort_upload(
                *req_.upload_id,
                [this, request = job.request](core::ports::CatalogResult<void> result) noexcept {
                    --pending_;
                    if (!serving(request)) {
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
    const auto token = claim_.token_of(request_seq_);
    if (offset == nullptr || id == nullptr || stored == nullptr || !token) {
        fail(Status::InternalServerError);
        return;
    }
    if (*offset != durable) {
        fail(Status::Conflict, durable);
        return;
    }
    // The client resumes where the store says; bring the catalog up to it first, so that a
    // commit or a HEAD after this request agrees with both.
    ++pending_;
    deps().catalog.record_progress(
        *id, *token, stored->upload.video_id, durable,
        [this, durable, request = request_seq_](core::ports::CatalogResult<void> result) noexcept {
            --pending_;
            if (!serving(request)) {
                return;
            }
            if (!result) {
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
        [this, video](core::ports::CatalogResult<core::VideoState> result) noexcept {
            --pending_;
            if (phase_ != Phase::Request) {
                return;
            }
            if (!result) {
                fail_catalog(result.error());
                return;
            }
            // Read in the commit's own transaction, so a commit that went through is never
            // answered with an error: processing the first time, and on a repeat whatever the
            // worker has made of the video since.
            respond_json(Status::Ok, std::format(R"({{"video_id":"{}","state":"{}"}})",
                                                 video.to_string(), state_name(*result)));
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
        head.retry_after = req_.retry_after.value_or(kRetryAfter);
    }
    if (status == Status::Unauthorized) {
        head.www_authenticate = req_.token.empty() ? "Bearer" : R"(Bearer error="invalid_token")";
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

// A stall that ends the request (the store fails the part, the backstop, the client leaves) is
// the longest there is, and must be counted with the rest.
void Connection::end_stall() noexcept {
    if (req_.stalled_since) {
        gw().backend_write_stall().observe(
            std::chrono::duration_cast<core::Millis>(now() - *req_.stalled_since));
        req_.stalled_since.reset();
    }
}

void Connection::respond(http::ResponseHead head, std::string_view body) noexcept {
    if (req_.responded || phase_ != Phase::Request) {
        return;
    }
    req_.responded = true;
    end_stall();
    if (session_) {
        session_->abort();
        session_.reset();
    }
    release_slot();
    release_request_hold();
    settle_upload_bytes();
    const bool keep = req_.keep_alive && req_.message_complete && !draining_;
    head.connection = keep ? http::Connection::KeepAlive : http::Connection::Close;
    head.request_id = request_id();
    record_response(head.status);
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

// Counts the response and logs it in one line, without the target, headers or body: tokens
// travel in headers, and ids are all a trace needs.
void Connection::record_response(http::Status status) noexcept {
    const std::uint16_t code = http::code(status);
    const std::size_t status_class = code / 100;
    if (status_class >= 1 && status_class <= 5) {
        ++gw().counters().responses.at(status_class - 1);
    }
    ops::Level level = code >= 500 ? ops::Level::Warn : ops::Level::Info;
    // Probes and scrapes arrive every few seconds and say nothing about traffic.
    if (req_.route == RouteId::Healthz || req_.route == RouteId::Readyz ||
        req_.route == RouteId::Metrics) {
        level = ops::Level::Debug;
    }
    const auto ms = std::chrono::duration_cast<core::Millis>(now() - request_started_);
    deps().log.log(level, "request",
                   {{"request_id", request_id()},
                    {"method", req_.method ? method_name(*req_.method) : "-"},
                    {"route", req_.route ? to_string(*req_.route) : "-"},
                    {"status", code},
                    {"ms", ms.count()},
                    {"bytes_in", req_.content_length}});
}

void Connection::finish_request() noexcept {
    // A request gives its claim back when it ends, by whichever path it ends.
    release_claim();
    // Only the request that just ended could have held one.
    assert(!claim_.held() && "a request finished while a claim was held");
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
    resume();
}

bool Connection::response_held_back() const noexcept {
    // An io_uring send still in flight counts here as held back; its completion drains the
    // queue and on_writable resumes at once.
    if (transport_->pending_send_bytes() > 0) {
        return true;
    }
    // Without TCP_INFO (or a kernel that fills in too little of it) only the transport is
    // looked at; such a connection kept the kernel's user timeout, which still bounds it.
    const auto progress = net::send_progress(fd_);
    return progress && progress->unsent;
}

bool Connection::hold_back_request() noexcept {
    // A new request is not read while the last response is held back, in the transport or in
    // the kernel behind a shut window: a client that keeps asking and never reads would
    // otherwise queue responses without end, each request restarting the header timeout.
    // The request waits, and the header timeout keeps counting from that response, so a
    // client that does not take it within the timeout is closed (ADR-0071). A client whose
    // response has gone when it asks again, as any client that reads it has, does not wait
    // for a drain check; on io_uring a send not yet completed holds it until the next turn.
    if (!response_held_back()) {
        return false;
    }
    awaiting_drain_ = true;
    // The timer armed below replaces a zero-delay resume finish_request may have armed, so
    // only the drain check or on_writable resumes, and each ends the hold first.
    resume_pending_ = false;
    if (receiving_) {
        receiving_ = false;
        transport_->stop_receiving();
    }
    const auto idle = std::chrono::duration_cast<core::Millis>(now() - last_activity_);
    arm_timer(std::clamp(gw().limits().header_timeout - idle, core::Millis{0}, kDrainCheck));
    return true;
}

void Connection::resume() noexcept {
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

void Connection::release_request_hold() noexcept {
    if (request_hold_) {
        gateway_.release_client(*request_hold_);
        request_hold_.reset();
    }
}

void Connection::settle_upload_bytes() noexcept {
    const core::ports::Claims* claims = get(req_.claims);
    if (claims != nullptr && req_.bytes_charged > req_.bytes_received) {
        gateway_.refund_upload_bytes(claims->subject, req_.bytes_charged - req_.bytes_received);
    }
    req_.bytes_charged = 0;
}

void Connection::release_client_holds() noexcept {
    for (std::optional<ClientHold>* hold : {&connection_hold_, &request_hold_}) {
        if (*hold) {
            gateway_.release_client(**hold);
            hold->reset();
        }
    }
}

void Connection::release_claim() noexcept {
    claim_.release(request_seq_);
}

void Connection::linger() noexcept {
    phase_ = Phase::Lingering;
    // Whatever the client asked next goes unanswered: nothing is parsed from here on.
    resume_pending_ = false;
    awaiting_drain_ = false;
    held_.clear();
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
    end_stall();
    if (session_) {
        session_->abort();
        session_.reset();
    }
    // Whoever holds it: nothing on this connection will be served again.
    claim_.release_any();
    release_slot();
    release_client_holds();
    settle_upload_bytes();
    if (key_wait_) {
        deps().verifier.cancel_wait(*this);
        key_wait_ = false;
        --pending_;
    }
    deps().reactor.cancel_timer(timer_);
    timer_ = {};
    // A response the transport still holds part of is cut off whatever happens, so the close
    // is a reset, which drops what the kernel holds too, at once. One the kernel holds all of
    // is finished by the kernel after a FIN, as before the user timeout was cleared, and the
    // timeout is set again to bound that orphan: without it the kernel keeps one for as long
    // as the peer answers its probes of a shut window (ADR-0071).
    // An io_uring send still in flight counts as held by the transport, so such a close errs
    // toward a reset.
    if (transport_->pending_send_bytes() > 0 ||
        (output_waiting() && !net::restore_user_timeout(fd_))) {
        net::abort_on_close(fd_);
    }
    transport_->begin_close();
    gw().retire(handle_);
}

bool Connection::output_waiting() const noexcept {
    if (transport_->pending_send_bytes() > 0) {
        return true;
    }
    // Without TCP_INFO the kernel's user timeout was left in place, and bounds the orphan.
    const auto progress = net::send_progress(fd_);
    return progress && progress->waiting;
}

void Connection::on_writable() noexcept {
    if (awaiting_drain_ && phase_ == Phase::Idle && !response_held_back()) {
        awaiting_drain_ = false;
        resume();
    }
}

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

void Connection::on_idle_timeout(core::MonoTime t) noexcept {
    const core::Millis header_timeout = gw().limits().header_timeout;
    const auto idle = std::chrono::duration_cast<core::Millis>(t - last_activity_);
    if (idle >= header_timeout) {
        ++gw().counters().timeouts_header;
        close();
        return;
    }
    // The kernel says nothing when a shut window lets the rest of a response go, so a
    // held-back response is looked at again every kDrainCheck.
    if (awaiting_drain_) {
        if (!response_held_back()) {
            awaiting_drain_ = false;
            resume();
            return;
        }
        arm_timer(std::min(kDrainCheck, header_timeout - idle));
        return;
    }
    arm_timer(header_timeout - idle);
}

void Connection::read_next_request() noexcept {
    // A request the client pipelined behind a response still held back waits too.
    if (phase_ == Phase::Idle && (!held_.empty() || !parser_.unparsed().empty()) &&
        hold_back_request()) {
        return;
    }
    // Counted from the end of the last response, however long the client took to read it.
    const auto idle = std::chrono::duration_cast<core::Millis>(now() - last_activity_);
    arm_timer(std::max(gw().limits().header_timeout - idle, core::Millis{0}));
    // The parser resumes first: after a request it refused at the head, reading went on, so
    // bytes may have been held while it still waited to resume, and fed to it before then
    // they would only be kept, with nothing left to resume it. What it kept came first.
    const http::ParseResult resumed = parser_.resume();
    if (held_.empty()) {
        on_parse(resumed);
        return;
    }
    const std::vector<std::byte> bytes = std::exchange(held_, {});
    if (resumed == http::ParseProgress::NeedMore && phase_ == Phase::Idle) {
        // The request's header timeout counts from here, as it did when on_data replayed it.
        parse(bytes);
        return;
    }
    // A request it kept goes first, and the held bytes queue behind it: in the parser, or held
    // again if its response is held back in turn.
    on_parse(resumed);
    if (phase_ == Phase::Idle || phase_ == Phase::Request) {
        on_data(bytes);
    }
}

void Connection::on_timeout() noexcept {
    timer_ = {};
    if (phase_ == Phase::Closed) {
        return;
    }
    if (phase_ == Phase::Lingering) {
        close();
        return;
    }
    if (resume_pending_) {
        resume_pending_ = false;
        read_next_request();
        return;
    }
    const core::MonoTime t = now();
    const Limits& limits = gw().limits();
    if (phase_ == Phase::Idle) {
        on_idle_timeout(t);
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
        // Bytes waiting in staging mean the store is holding the body up, and the client is
        // backpressured, not idle. Only the store can tell a slow store from a broken one, so
        // the store ends a wait that goes wrong (ADR-0045).
        if (staging_head_ < staging_.size()) {
            arm_timer(std::min(limits.request_backstop - age, limits.body_idle_timeout));
            return;
        }
        const auto idle = std::chrono::duration_cast<core::Millis>(t - last_progress_);
        if (idle >= limits.body_idle_timeout) {
            ++gw().counters().timeouts_body;
            fail(Status::RequestTimeout);
            return;
        }
        core::Millis next = limits.body_idle_timeout - idle;
        // While the parser is paused the store is what holds the body up, and the window
        // skips that time when reading resumes.
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
