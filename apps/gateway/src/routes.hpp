#pragma once

#include "http/router.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace gateway {

enum class RouteId : std::uint8_t {
    CreateUpload,
    AppendChunk,
    UploadOffset,
    CancelUpload,
    CommitUpload,
    GetVideo,
    MasterPlaylist,
    MediaPlaylist,
    LivePlaylist,
    CreateStream,
    StreamStatus,
    StreamTicket,
    StartStream,
    EndStream,
    Healthz,
    Readyz,
    Metrics,
};

inline constexpr std::array<http::Route<RouteId>, 19> kRoutes{{
    {.method = http::Method::Post, .pattern = "/api/v1/uploads", .id = RouteId::CreateUpload},
    {.method = http::Method::Patch, .pattern = "/api/v1/uploads/{id}", .id = RouteId::AppendChunk},
    {.method = http::Method::Head, .pattern = "/api/v1/uploads/{id}", .id = RouteId::UploadOffset},
    {.method = http::Method::Delete,
     .pattern = "/api/v1/uploads/{id}",
     .id = RouteId::CancelUpload},
    {.method = http::Method::Post,
     .pattern = "/api/v1/uploads/{id}/commit",
     .id = RouteId::CommitUpload},
    {.method = http::Method::Get, .pattern = "/api/v1/videos/{id}", .id = RouteId::GetVideo},
    {.method = http::Method::Get,
     .pattern = "/api/v1/videos/{id}/master.m3u8",
     .id = RouteId::MasterPlaylist},
    {.method = http::Method::Get,
     .pattern = "/api/v1/videos/{id}/{rendition}/index.m3u8",
     .id = RouteId::MediaPlaylist},
    {.method = http::Method::Get,
     .pattern = "/api/v1/live/{id}/index.m3u8",
     .id = RouteId::LivePlaylist},
    // The stream service (ADR-0092).
    {.method = http::Method::Post, .pattern = "/api/v1/live", .id = RouteId::CreateStream},
    {.method = http::Method::Get, .pattern = "/api/v1/live/{id}", .id = RouteId::StreamStatus},
    {.method = http::Method::Post,
     .pattern = "/api/v1/live/{id}/ticket",
     .id = RouteId::StreamTicket},
    {.method = http::Method::Post,
     .pattern = "/api/v1/live/{id}/start",
     .id = RouteId::StartStream},
    {.method = http::Method::Post, .pattern = "/api/v1/live/{id}/end", .id = RouteId::EndStream},
    {.method = http::Method::Get, .pattern = "/api/v1/healthz", .id = RouteId::Healthz},
    {.method = http::Method::Get, .pattern = "/api/v1/readyz", .id = RouteId::Readyz},
    // The names a kubelet or a load balancer probes by default, outside the API prefix.
    {.method = http::Method::Get, .pattern = "/healthz", .id = RouteId::Healthz},
    {.method = http::Method::Get, .pattern = "/readyz", .id = RouteId::Readyz},
    {.method = http::Method::Get, .pattern = "/metrics", .id = RouteId::Metrics},
}};

inline constexpr http::Router<RouteId> kRouter{kRoutes};

// For the request log.
[[nodiscard]] constexpr std::string_view to_string(RouteId r) noexcept {
    switch (r) {
    case RouteId::CreateUpload:
        return "create_upload";
    case RouteId::AppendChunk:
        return "append_chunk";
    case RouteId::UploadOffset:
        return "upload_offset";
    case RouteId::CancelUpload:
        return "cancel_upload";
    case RouteId::CommitUpload:
        return "commit_upload";
    case RouteId::GetVideo:
        return "get_video";
    case RouteId::MasterPlaylist:
        return "master_playlist";
    case RouteId::MediaPlaylist:
        return "media_playlist";
    case RouteId::LivePlaylist:
        return "live_playlist";
    case RouteId::CreateStream:
        return "create_stream";
    case RouteId::StreamStatus:
        return "stream_status";
    case RouteId::StreamTicket:
        return "stream_ticket";
    case RouteId::StartStream:
        return "start_stream";
    case RouteId::EndStream:
        return "end_stream";
    case RouteId::Healthz:
        return "healthz";
    case RouteId::Readyz:
        return "readyz";
    case RouteId::Metrics:
        return "metrics";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool requires_auth(RouteId r) noexcept {
    switch (r) {
    case RouteId::Healthz:
    case RouteId::Readyz:
    case RouteId::Metrics:
        return false;
    case RouteId::CreateUpload:
    case RouteId::AppendChunk:
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
        return true;
    }
    return true;
}

} // namespace gateway
