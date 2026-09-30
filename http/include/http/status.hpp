#pragma once

#include <cstdint>
#include <string_view>

namespace http {

enum class Status : std::uint16_t {
    Ok = 200,
    Created = 201,
    NoContent = 204,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    ContentTooLarge = 413,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HttpVersionNotSupported = 505,
};

[[nodiscard]] constexpr std::uint16_t code(Status s) noexcept {
    return static_cast<std::uint16_t>(s);
}

// RFC 9110 section 15.
[[nodiscard]] constexpr std::string_view reason_phrase(Status s) noexcept {
    switch (s) {
    case Status::Ok:
        return "OK";
    case Status::Created:
        return "Created";
    case Status::NoContent:
        return "No Content";
    case Status::BadRequest:
        return "Bad Request";
    case Status::Unauthorized:
        return "Unauthorized";
    case Status::Forbidden:
        return "Forbidden";
    case Status::NotFound:
        return "Not Found";
    case Status::MethodNotAllowed:
        return "Method Not Allowed";
    case Status::RequestTimeout:
        return "Request Timeout";
    case Status::Conflict:
        return "Conflict";
    case Status::Gone:
        return "Gone";
    case Status::LengthRequired:
        return "Length Required";
    case Status::ContentTooLarge:
        return "Content Too Large";
    case Status::TooManyRequests:
        return "Too Many Requests";
    case Status::RequestHeaderFieldsTooLarge:
        return "Request Header Fields Too Large";
    case Status::InternalServerError:
        return "Internal Server Error";
    case Status::NotImplemented:
        return "Not Implemented";
    case Status::ServiceUnavailable:
        return "Service Unavailable";
    case Status::HttpVersionNotSupported:
        return "HTTP Version Not Supported";
    }
    // The reason phrase is optional on the wire (RFC 9112 section 4).
    return {};
}

} // namespace http
