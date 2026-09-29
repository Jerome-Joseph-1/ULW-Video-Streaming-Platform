#include "multipart.hpp"

#include "infra/curl/http.hpp"

#include "failure.hpp"

#include <array>
#include <utility>

namespace infra::storage::s3 {

using core::ports::StorageError;

std::expected<std::string, StorageError> initiate_upload(const Control& control,
                                                         const s3util::Bucket& bucket,
                                                         const core::StorageKey& key,
                                                         const core::ContentType& type) {
    const std::array headers{
        s3util::Header{.name = "content-type", .value = std::string(type.view())}};
    const auto target = bucket.object(key, {{.name = "uploads", .value = ""}});
    return control.retrying<std::string>([&]() -> std::expected<std::string, Failed> {
        auto response = control.send(curl::Method::Post, target, headers, {}, kMaxDocumentBytes);
        if (!response) {
            return std::unexpected(response.error());
        }
        auto parsed = s3util::parse_initiate_multipart_upload(response->body);
        if (!parsed) {
            return std::unexpected(failed(parsed.error()));
        }
        return std::move(parsed->upload_id);
    });
}

std::expected<void, StorageError>
complete_upload(const Control& control, const s3util::Bucket& bucket, const core::StorageKey& key,
                const std::string& upload_id, std::span<const s3util::CompletedPart> parts) {
    const std::string body = s3util::complete_multipart_upload_body(parts);
    const std::array headers{s3util::Header{.name = "content-type", .value = "application/xml"}};
    const auto target = bucket.object(key, {{.name = "uploadId", .value = upload_id}});
    return control.retrying<void>([&]() -> std::expected<void, Failed> {
        auto response = control.send(curl::Method::Post, target, headers,
                                     std::as_bytes(std::span(body)), kMaxDocumentBytes);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (auto parsed = s3util::parse_complete_multipart_upload(response->body); !parsed) {
            return std::unexpected(failed(parsed.error()));
        }
        return {};
    });
}

std::expected<void, StorageError> abort_upload(const Control& control, const s3util::Bucket& bucket,
                                               const core::StorageKey& key,
                                               const std::string& upload_id) {
    const auto target = bucket.object(key, {{.name = "uploadId", .value = upload_id}});
    return control.retrying<void>([&]() -> std::expected<void, Failed> {
        auto response = control.send(curl::Method::Delete, target, {}, {}, 0);
        if (!response) {
            return std::unexpected(response.error());
        }
        return {};
    });
}

} // namespace infra::storage::s3
