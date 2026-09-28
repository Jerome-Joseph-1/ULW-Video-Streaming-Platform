#include "infra/s3util/xml.hpp"

#include "core/util/time.hpp"

#include "decimal.hpp"
#include "xml_tree.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::s3util {

namespace {

using detail::XmlDocument;
using detail::XmlNode;

// S3 numbers parts 1..10000 inclusive.
constexpr std::uint64_t kMaxPartNumber = 10'000;

std::expected<std::optional<std::string>, XmlError>
optional_text(const XmlDocument& doc, const XmlNode& parent, std::string_view name) {
    const auto node = doc.find(parent, name);
    if (!node) {
        return std::unexpected(node.error());
    }
    if (*node == nullptr) {
        return std::nullopt;
    }
    if ((*node)->first_child != detail::kNoNode) {
        return std::unexpected(XmlError::InvalidValue);
    }
    return detail::decode_text((*node)->text);
}

std::expected<std::string, XmlError> required_text(const XmlDocument& doc, const XmlNode& parent,
                                                   std::string_view name) {
    return optional_text(doc, parent, name)
        .and_then([](std::optional<std::string> text) -> std::expected<std::string, XmlError> {
            if (!text) {
                return std::unexpected(XmlError::MissingElement);
            }
            return std::move(*text);
        });
}

bool is_present(const std::optional<std::string>& text) noexcept {
    return text && !text->empty();
}

// Upload ids, keys, ETags and error codes are never legitimately empty, and an empty one fed
// back into a later request would address something other than intended.
std::expected<std::string, XmlError> required_id(const XmlDocument& doc, const XmlNode& parent,
                                                 std::string_view name) {
    return required_text(doc, parent, name)
        .and_then([](std::string s) -> std::expected<std::string, XmlError> {
            if (s.empty()) {
                return std::unexpected(XmlError::InvalidValue);
            }
            return s;
        });
}

std::expected<std::uint64_t, XmlError> to_u64(std::string_view text) {
    if (const auto value = detail::parse_decimal<std::uint64_t>(text)) {
        return *value;
    }
    return std::unexpected(XmlError::InvalidValue);
}

// `min` is 1 for a part and 0 for a marker, which may point before the first part.
std::expected<std::uint32_t, XmlError> to_part_number(std::string_view text, std::uint64_t min) {
    return to_u64(text).and_then([min](std::uint64_t n) -> std::expected<std::uint32_t, XmlError> {
        if (n < min || n > kMaxPartNumber) {
            return std::unexpected(XmlError::InvalidValue);
        }
        return static_cast<std::uint32_t>(n);
    });
}

std::expected<std::optional<std::uint32_t>, XmlError>
optional_marker(const XmlDocument& doc, const XmlNode& parent, std::string_view name) {
    return optional_text(doc, parent, name)
        .and_then([](const std::optional<std::string>& text)
                      -> std::expected<std::optional<std::uint32_t>, XmlError> {
            if (!text) {
                return std::nullopt;
            }
            return to_part_number(*text, 0);
        });
}

std::expected<bool, XmlError> required_bool(const XmlDocument& doc, const XmlNode& parent,
                                            std::string_view name) {
    return required_text(doc, parent, name)
        .and_then([](const std::string& s) -> std::expected<bool, XmlError> {
            if (s == "true") {
                return true;
            }
            if (s == "false") {
                return false;
            }
            return std::unexpected(XmlError::InvalidValue);
        });
}

// "2010-11-10T20:48:33.000Z": what every S3 implementation writes. Only UTC is accepted, and
// the fraction may be omitted or have up to nanosecond precision.
std::optional<core::WallTime> parse_timestamp(std::string_view s) {
    constexpr std::size_t kSecondsEnd = std::string_view("2010-11-10T20:48:33").size();
    if (s.size() <= kSecondsEnd || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':' || !s.ends_with('Z')) {
        return std::nullopt;
    }
    const auto year = detail::parse_decimal<std::uint16_t>(s.substr(0, 4));
    const auto month = detail::parse_decimal<std::uint8_t>(s.substr(5, 2));
    const auto day = detail::parse_decimal<std::uint8_t>(s.substr(8, 2));
    const auto hour = detail::parse_decimal<std::uint8_t>(s.substr(11, 2));
    const auto minute = detail::parse_decimal<std::uint8_t>(s.substr(14, 2));
    const auto second = detail::parse_decimal<std::uint8_t>(s.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
        *second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{*month},
                                           std::chrono::day{*day}};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::chrono::nanoseconds fraction{0};
    const std::string_view rest = s.substr(kSecondsEnd, s.size() - kSecondsEnd - 1);
    if (!rest.empty()) {
        // 9 digits: nanoseconds, the finest a WallTime holds.
        const std::string_view digits = rest.substr(1);
        const auto value = detail::parse_decimal<std::uint32_t>(digits);
        if (!rest.starts_with('.') || digits.empty() || digits.size() > 9 || !value) {
            return std::nullopt;
        }
        std::int64_t nanos = *value;
        for (std::size_t i = digits.size(); i < 9; ++i) {
            nanos *= 10;
        }
        fraction = std::chrono::nanoseconds{nanos};
    }
    using std::chrono::floor;
    return core::WallTime{std::chrono::sys_days{date}} + std::chrono::hours{*hour} +
           std::chrono::minutes{*minute} + std::chrono::seconds{*second} +
           floor<core::WallTime::duration>(fraction);
}

std::expected<ErrorDocument, XmlError> read_error(const XmlDocument& doc, const XmlNode& root) {
    auto code = required_id(doc, root, "Code");
    if (!code) {
        return std::unexpected(code.error());
    }
    auto message = optional_text(doc, root, "Message");
    if (!message) {
        return std::unexpected(message.error());
    }
    return ErrorDocument{.code = std::move(*code), .message = std::move(*message).value_or("")};
}

template <typename T>
using Reader = std::expected<T, XmlError> (*)(const XmlDocument&, const XmlNode&);

template <typename T>
std::expected<T, ResponseError> parse_response(std::string_view root_name, Reader<T> read,
                                               std::string_view body) {
    const auto doc = XmlDocument::parse(body);
    if (!doc) {
        return std::unexpected(ResponseError{doc.error()});
    }
    const XmlNode& root = doc->root();
    if (root.name == "Error") {
        auto error = read_error(*doc, root);
        if (!error) {
            return std::unexpected(ResponseError{error.error()});
        }
        return std::unexpected(ResponseError{std::move(*error)});
    }
    if (root.name != root_name) {
        return std::unexpected(ResponseError{XmlError::UnexpectedRoot});
    }
    auto result = read(*doc, root);
    if (!result) {
        return std::unexpected(ResponseError{result.error()});
    }
    return std::move(*result);
}

std::expected<InitiateMultipartUploadResult, XmlError> read_initiate(const XmlDocument& doc,
                                                                     const XmlNode& root) {
    return required_id(doc, root, "UploadId").transform([](std::string id) {
        return InitiateMultipartUploadResult{.upload_id = std::move(id)};
    });
}

std::expected<UploadedPart, XmlError> read_part(const XmlDocument& doc, const XmlNode& node) {
    const auto number = required_text(doc, node, "PartNumber").and_then([](const std::string& s) {
        return to_part_number(s, 1);
    });
    if (!number) {
        return std::unexpected(number.error());
    }
    auto etag = required_id(doc, node, "ETag");
    if (!etag) {
        return std::unexpected(etag.error());
    }
    const auto size =
        required_text(doc, node, "Size").and_then([](const std::string& s) { return to_u64(s); });
    if (!size) {
        return std::unexpected(size.error());
    }
    return UploadedPart{.part_number = *number, .etag = std::move(*etag), .size = *size};
}

std::expected<ListPartsResult, XmlError> read_list_parts(const XmlDocument& doc,
                                                         const XmlNode& root) {
    ListPartsResult result;
    const auto truncated = required_bool(doc, root, "IsTruncated");
    if (!truncated) {
        return std::unexpected(truncated.error());
    }
    result.is_truncated = *truncated;
    const auto marker = optional_marker(doc, root, "NextPartNumberMarker");
    if (!marker) {
        return std::unexpected(marker.error());
    }
    // A truncated listing without a marker would restart from the top forever.
    if (result.is_truncated && !marker->has_value()) {
        return std::unexpected(XmlError::MissingElement);
    }
    result.next_part_number_marker = *marker;
    for (const XmlNode* child = doc.first_child(root); child != nullptr;
         child = doc.next_sibling(*child)) {
        if (child->name != "Part") {
            continue;
        }
        auto part = read_part(doc, *child);
        if (!part) {
            return std::unexpected(part.error());
        }
        result.parts.push_back(std::move(*part));
    }
    return result;
}

std::expected<CompleteMultipartUploadResult, XmlError> read_complete(const XmlDocument& doc,
                                                                     const XmlNode& root) {
    return required_id(doc, root, "ETag").transform([](std::string etag) {
        return CompleteMultipartUploadResult{.etag = std::move(etag)};
    });
}

std::expected<ListObjectsResult, XmlError> read_list_objects(const XmlDocument& doc,
                                                             const XmlNode& root) {
    ListObjectsResult result;
    const auto truncated = required_bool(doc, root, "IsTruncated");
    if (!truncated) {
        return std::unexpected(truncated.error());
    }
    result.is_truncated = *truncated;
    auto token = optional_text(doc, root, "NextContinuationToken");
    if (!token) {
        return std::unexpected(token.error());
    }
    if (result.is_truncated && !is_present(*token)) {
        return std::unexpected(XmlError::MissingElement);
    }
    result.next_continuation_token = std::move(*token);
    for (const XmlNode* child = doc.first_child(root); child != nullptr;
         child = doc.next_sibling(*child)) {
        if (child->name != "Contents") {
            continue;
        }
        auto key = required_id(doc, *child, "Key");
        if (!key) {
            return std::unexpected(key.error());
        }
        result.keys.push_back(std::move(*key));
    }
    return result;
}

std::expected<MultipartUpload, XmlError> read_upload(const XmlDocument& doc, const XmlNode& node) {
    auto key = required_id(doc, node, "Key");
    if (!key) {
        return std::unexpected(key.error());
    }
    auto upload_id = required_id(doc, node, "UploadId");
    if (!upload_id) {
        return std::unexpected(upload_id.error());
    }
    const auto initiated =
        required_text(doc, node, "Initiated")
            .and_then([](const std::string& s) -> std::expected<core::WallTime, XmlError> {
                if (const auto t = parse_timestamp(s)) {
                    return *t;
                }
                return std::unexpected(XmlError::InvalidValue);
            });
    if (!initiated) {
        return std::unexpected(initiated.error());
    }
    return MultipartUpload{
        .key = std::move(*key), .upload_id = std::move(*upload_id), .initiated = *initiated};
}

std::expected<ListMultipartUploadsResult, XmlError> read_list_uploads(const XmlDocument& doc,
                                                                      const XmlNode& root) {
    ListMultipartUploadsResult result;
    const auto truncated = required_bool(doc, root, "IsTruncated");
    if (!truncated) {
        return std::unexpected(truncated.error());
    }
    result.is_truncated = *truncated;
    auto key_marker = optional_text(doc, root, "NextKeyMarker");
    if (!key_marker) {
        return std::unexpected(key_marker.error());
    }
    auto upload_id_marker = optional_text(doc, root, "NextUploadIdMarker");
    if (!upload_id_marker) {
        return std::unexpected(upload_id_marker.error());
    }
    if (result.is_truncated && (!is_present(*key_marker) || !is_present(*upload_id_marker))) {
        return std::unexpected(XmlError::MissingElement);
    }
    result.next_key_marker = std::move(*key_marker);
    result.next_upload_id_marker = std::move(*upload_id_marker);
    for (const XmlNode* child = doc.first_child(root); child != nullptr;
         child = doc.next_sibling(*child)) {
        if (child->name != "Upload") {
            continue;
        }
        auto upload = read_upload(doc, *child);
        if (!upload) {
            return std::unexpected(upload.error());
        }
        result.uploads.push_back(std::move(*upload));
    }
    return result;
}

void append_escaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
        case '&':
            out.append("&amp;");
            break;
        case '<':
            out.append("&lt;");
            break;
        case '>':
            out.append("&gt;");
            break;
        case '"':
            out.append("&quot;");
            break;
        case '\'':
            out.append("&apos;");
            break;
        default:
            out.push_back(c);
        }
    }
}

} // namespace

std::expected<ErrorDocument, XmlError> parse_error_document(std::string_view body) {
    const auto doc = XmlDocument::parse(body);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    if (doc->root().name != "Error") {
        return std::unexpected(XmlError::UnexpectedRoot);
    }
    return read_error(*doc, doc->root());
}

std::expected<InitiateMultipartUploadResult, ResponseError>
parse_initiate_multipart_upload(std::string_view body) {
    return parse_response<InitiateMultipartUploadResult>("InitiateMultipartUploadResult",
                                                         read_initiate, body);
}

std::expected<ListPartsResult, ResponseError> parse_list_parts(std::string_view body) {
    return parse_response<ListPartsResult>("ListPartsResult", read_list_parts, body);
}

std::expected<CompleteMultipartUploadResult, ResponseError>
parse_complete_multipart_upload(std::string_view body) {
    return parse_response<CompleteMultipartUploadResult>("CompleteMultipartUploadResult",
                                                         read_complete, body);
}

std::expected<ListObjectsResult, ResponseError> parse_list_objects_v2(std::string_view body) {
    return parse_response<ListObjectsResult>("ListBucketResult", read_list_objects, body);
}

std::expected<ListMultipartUploadsResult, ResponseError>
parse_list_multipart_uploads(std::string_view body) {
    return parse_response<ListMultipartUploadsResult>("ListMultipartUploadsResult",
                                                      read_list_uploads, body);
}

std::string complete_multipart_upload_body(std::span<const CompletedPart> parts) {
    std::vector<const CompletedPart*> ordered;
    ordered.reserve(parts.size());
    for (const auto& part : parts) {
        ordered.push_back(&part);
    }
    std::ranges::stable_sort(ordered, {}, &CompletedPart::part_number);

    std::string body =
        R"(<CompleteMultipartUpload xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)";
    for (const CompletedPart* part : ordered) {
        body.append("<Part><PartNumber>").append(std::to_string(part->part_number));
        body.append("</PartNumber><ETag>");
        append_escaped(body, part->etag);
        body.append("</ETag></Part>");
    }
    body.append("</CompleteMultipartUpload>");
    return body;
}

} // namespace infra::s3util
