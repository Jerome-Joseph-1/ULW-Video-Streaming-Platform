#include "http/method.hpp"
#include "http/response.hpp"
#include "http/status.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using http::Connection;
using http::Method;
using http::ResponseHead;
using http::Status;
using http::WriteError;

constexpr std::array kAllStatuses{
    Status::Ok,
    Status::Created,
    Status::NoContent,
    Status::BadRequest,
    Status::Forbidden,
    Status::NotFound,
    Status::MethodNotAllowed,
    Status::RequestTimeout,
    Status::Conflict,
    Status::LengthRequired,
    Status::ContentTooLarge,
    Status::RequestHeaderFieldsTooLarge,
    Status::InternalServerError,
    Status::NotImplemented,
    Status::ServiceUnavailable,
    Status::HttpVersionNotSupported,
};

std::string write(const ResponseHead& head) {
    std::array<char, 1024> buffer{};
    const auto written = http::write_response_head(head, buffer);
    EXPECT_TRUE(written.has_value());
    return written ? std::string{buffer.data(), *written} : std::string{};
}

TEST(FixedResponse, IsACompleteBodilessMessage) {
    EXPECT_EQ(http::fixed_response(Status::BadRequest, Connection::Close),
              "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(http::fixed_response(Status::NotFound, Connection::KeepAlive),
              "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
    EXPECT_EQ(http::fixed_response(Status::RequestHeaderFieldsTooLarge, Connection::Close),
              "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\n"
              "Connection: close\r\n\r\n");
}

TEST(FixedResponse, NoContentCarriesNoContentLength) {
    EXPECT_EQ(http::fixed_response(Status::NoContent, Connection::KeepAlive),
              "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n");
}

TEST(FixedResponse, MatchesTheWriterForEveryStatus) {
    for (const Status s : kAllStatuses) {
        for (const Connection c : {Connection::KeepAlive, Connection::Close}) {
            EXPECT_EQ(http::fixed_response(s, c), write({.status = s, .connection = c}))
                << http::code(s);
        }
    }
}

TEST(ResponseWriter, WritesAPlaylistHead) {
    EXPECT_EQ(write({.status = Status::Ok,
                     .content_length = 1234,
                     .content_type = "application/vnd.apple.mpegurl",
                     .cache_control = "max-age=2",
                     .request_id = "r-1"}),
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 1234\r\n"
              "Connection: keep-alive\r\n"
              "Content-Type: application/vnd.apple.mpegurl\r\n"
              "Cache-Control: max-age=2\r\n"
              "X-Request-Id: r-1\r\n"
              "\r\n");
}

TEST(ResponseWriter, WritesAnUploadCreation) {
    EXPECT_EQ(write({.status = Status::Created,
                     .location = "/api/v1/uploads/0192f3c4",
                     .upload_offset = 0}),
              "HTTP/1.1 201 Created\r\n"
              "Content-Length: 0\r\n"
              "Connection: keep-alive\r\n"
              "Location: /api/v1/uploads/0192f3c4\r\n"
              "Upload-Offset: 0\r\n"
              "\r\n");
}

TEST(ResponseWriter, NoContentOmitsContentLengthEvenIfOneIsGiven) {
    EXPECT_EQ(write({.status = Status::NoContent, .content_length = 7, .upload_offset = 16777216}),
              "HTTP/1.1 204 No Content\r\n"
              "Connection: keep-alive\r\n"
              "Upload-Offset: 16777216\r\n"
              "\r\n");
}

TEST(ResponseWriter, ListsAllowedMethodsInAFixedOrder) {
    EXPECT_EQ(write({.status = Status::MethodNotAllowed,
                     .allow = {Method::Delete, Method::Patch, Method::Head}}),
              "HTTP/1.1 405 Method Not Allowed\r\n"
              "Content-Length: 0\r\n"
              "Connection: keep-alive\r\n"
              "Allow: HEAD, PATCH, DELETE\r\n"
              "\r\n");
}

TEST(ResponseWriter, WritesRetryAfterInSeconds) {
    EXPECT_EQ(write({.status = Status::ServiceUnavailable,
                     .connection = Connection::Close,
                     .retry_after = std::chrono::seconds{30}}),
              "HTTP/1.1 503 Service Unavailable\r\n"
              "Content-Length: 0\r\n"
              "Connection: close\r\n"
              "Retry-After: 30\r\n"
              "\r\n");
}

TEST(ResponseWriter, ReportsEveryBufferShorterThanTheHead) {
    const ResponseHead head{.status = Status::Created,
                            .location = "/api/v1/uploads/u",
                            .upload_offset = 42,
                            .allow = {Method::Get},
                            .request_id = "r-2"};
    const std::string expected = write(head);
    ASSERT_FALSE(expected.empty());

    std::vector<char> buffer(expected.size());
    for (std::size_t size = 0; size < expected.size(); ++size) {
        const auto written = http::write_response_head(head, std::span{buffer}.first(size));
        ASSERT_EQ(written, std::unexpected(WriteError::BufferTooSmall)) << size;
    }
    ASSERT_EQ(http::write_response_head(head, buffer), expected.size());
    EXPECT_EQ(std::string_view(buffer.data(), buffer.size()), expected);
}

TEST(ResponseWriter, RefusesValuesThatCouldEndTheFieldEarly) {
    const std::array<ResponseHead, 5> heads{{
        {.status = Status::Created, .location = "/a\r\nSet-Cookie: s=1"},
        {.status = Status::Ok, .request_id = "r\n"},
        {.status = Status::Ok, .content_type = std::string_view{"text/plain\0x", 12}},
        {.status = Status::Ok, .cache_control = "no-store\x7F"},
        {.status = Status::ServiceUnavailable, .retry_after = std::chrono::seconds{-1}},
    }};
    std::array<char, 1024> buffer{};
    for (const ResponseHead& head : heads) {
        EXPECT_EQ(http::write_response_head(head, buffer),
                  std::unexpected(WriteError::InvalidFieldValue));
    }
}

TEST(ResponseWriter, AcceptsTabsAndObsText) {
    EXPECT_EQ(write({.status = Status::Ok, .request_id = "a\tb\xC3\xA9"}),
              "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n"
              "X-Request-Id: a\tb\xC3\xA9\r\n\r\n");
}

} // namespace
