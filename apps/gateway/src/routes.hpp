#pragma once

#include "http/router.hpp"

#include <array>
#include <cstdint>

namespace gateway {

enum class RouteId : std::uint8_t {
    CreateUpload,
    AppendChunk,
    UploadOffset,
    CancelUpload,
    CommitUpload,
    GetVideo,
    Healthz,
    Readyz,
    Metrics,
};

inline constexpr std::array<http::Route<RouteId>, 9> kRoutes{{
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
    {.method = http::Method::Get, .pattern = "/api/v1/healthz", .id = RouteId::Healthz},
    {.method = http::Method::Get, .pattern = "/api/v1/readyz", .id = RouteId::Readyz},
    {.method = http::Method::Get, .pattern = "/metrics", .id = RouteId::Metrics},
}};

inline constexpr http::Router<RouteId> kRouter{kRoutes};

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
        return true;
    }
    return true;
}

} // namespace gateway
