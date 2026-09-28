#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace infra::s3util {

enum class XmlError : std::uint8_t {
    Malformed,
    UnexpectedRoot,
    MissingElement,
    DuplicateElement,
    InvalidValue,
};

struct ErrorDocument {
    std::string code;
    std::string message;
};

// A 2xx body can still be an <Error> document: S3 reports a late failure of
// CompleteMultipartUpload or a copy that way, because the status line has already gone out.
// Every success parser therefore returns one as an error of its own rather than a parse
// failure, and no 2xx body is taken on trust.
using ResponseError = std::variant<XmlError, ErrorDocument>;

struct InitiateMultipartUploadResult {
    std::string upload_id;
};

struct UploadedPart {
    std::uint32_t part_number = 0;
    // Exactly as S3 sent it, quotes included; CompleteMultipartUpload wants it back verbatim.
    std::string etag;
    std::uint64_t size = 0;
};

struct ListPartsResult {
    std::vector<UploadedPart> parts;
    bool is_truncated = false;
    // Always present when is_truncated is set.
    std::optional<std::uint32_t> next_part_number_marker;
};

struct CompleteMultipartUploadResult {
    std::string etag;
};

struct ListObjectsResult {
    std::vector<std::string> keys;
    bool is_truncated = false;
    // Always present when is_truncated is set.
    std::optional<std::string> next_continuation_token;
};

struct MultipartUpload {
    std::string key;
    std::string upload_id;
    core::WallTime initiated;
};

struct ListMultipartUploadsResult {
    std::vector<MultipartUpload> uploads;
    bool is_truncated = false;
    // Both present when is_truncated is set.
    std::optional<std::string> next_key_marker;
    std::optional<std::string> next_upload_id_marker;
};

[[nodiscard]] std::expected<ErrorDocument, XmlError> parse_error_document(std::string_view body);

[[nodiscard]] std::expected<InitiateMultipartUploadResult, ResponseError>
parse_initiate_multipart_upload(std::string_view body);

[[nodiscard]] std::expected<ListPartsResult, ResponseError> parse_list_parts(std::string_view body);

[[nodiscard]] std::expected<CompleteMultipartUploadResult, ResponseError>
parse_complete_multipart_upload(std::string_view body);

[[nodiscard]] std::expected<ListObjectsResult, ResponseError>
parse_list_objects_v2(std::string_view body);

[[nodiscard]] std::expected<ListMultipartUploadsResult, ResponseError>
parse_list_multipart_uploads(std::string_view body);

struct CompletedPart {
    std::uint32_t part_number = 0;
    std::string etag;
};

// Parts go out in ascending part-number order whatever order they are given in, since S3
// rejects anything else with InvalidPartOrder.
[[nodiscard]] std::string complete_multipart_upload_body(std::span<const CompletedPart> parts);

} // namespace infra::s3util
