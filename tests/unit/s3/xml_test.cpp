#include "core/util/time.hpp"
#include "infra/s3util/xml.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {

using infra::s3util::complete_multipart_upload_body;
using infra::s3util::CompletedPart;
using infra::s3util::ErrorDocument;
using infra::s3util::parse_complete_multipart_upload;
using infra::s3util::parse_error_document;
using infra::s3util::parse_initiate_multipart_upload;
using infra::s3util::parse_list_multipart_uploads;
using infra::s3util::parse_list_objects_v2;
using infra::s3util::parse_list_parts;
using infra::s3util::ResponseError;
using infra::s3util::XmlError;

// The documents below follow the response examples in the S3 API reference.

constexpr std::string_view kInitiate = R"(<?xml version="1.0" encoding="UTF-8"?>
<InitiateMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>example-bucket</Bucket>
  <Key>example-object</Key>
  <UploadId>VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA</UploadId>
</InitiateMultipartUploadResult>)";

constexpr std::string_view kListParts = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListPartsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>example-bucket</Bucket>
  <Key>example-object</Key>
  <UploadId>XXBsb2FkIElEIGZvciBlbHZpbmcncyVcdS1tb3ZpZS5tMnRzEEEwbG9hZA</UploadId>
  <Initiator>
    <ID>arn:aws:iam::111122223333:user/some-user-11116a31-17b5-4fb7-9df5-b288870f11xx</ID>
    <DisplayName>umat-user-11116a31-17b5-4fb7-9df5-b288870f11xx</DisplayName>
  </Initiator>
  <Owner>
    <ID>75aa57f09aa0c8caeab4f8c24e99d10f8e7faeebf76c078efc7c6caea54ba06a</ID>
    <DisplayName>someName</DisplayName>
  </Owner>
  <StorageClass>STANDARD</StorageClass>
  <PartNumberMarker>1</PartNumberMarker>
  <NextPartNumberMarker>3</NextPartNumberMarker>
  <MaxParts>2</MaxParts>
  <IsTruncated>true</IsTruncated>
  <Part>
    <PartNumber>2</PartNumber>
    <LastModified>2010-11-10T20:48:34.000Z</LastModified>
    <ETag>&quot;7778aef83f66abc1fa1e8477f296d394&quot;</ETag>
    <Size>10485760</Size>
  </Part>
  <Part>
    <PartNumber>3</PartNumber>
    <LastModified>2010-11-10T20:48:33.000Z</LastModified>
    <ETag>"aaaa18db4cc2f85cedef654fccc4a4x8"</ETag>
    <Size>10485760</Size>
  </Part>
</ListPartsResult>)";

constexpr std::string_view kComplete = R"(<?xml version="1.0" encoding="UTF-8"?>
<CompleteMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Location>https://example-bucket.s3.us-east-1.amazonaws.com/example-object</Location>
  <Bucket>example-bucket</Bucket>
  <Key>example-object</Key>
  <ETag>"3858f62230ac3c915f300c664312c11f-9"</ETag>
</CompleteMultipartUploadResult>)";

constexpr std::string_view kInternalError = R"(<?xml version="1.0" encoding="UTF-8"?>

<Error>
  <Code>InternalError</Code>
  <Message>We encountered an internal error. Please try again.</Message>
  <RequestId>656c76696e6727732072657175657374</RequestId>
  <HostId>Uuag1LuByRx9e6j5Onimru9pO4ZVKnJ2Qz7/C1NPcfTWAtRPfTaOFg==</HostId>
</Error>)";

constexpr std::string_view kListObjects = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>bucket</Name>
  <Prefix/>
  <KeyCount>2</KeyCount>
  <MaxKeys>2</MaxKeys>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken>
  <Contents>
    <Key>happyface.jpg</Key>
    <LastModified>2014-11-21T19:40:05.000Z</LastModified>
    <ETag>"70ee1738b6b21e2c8a43f3a5ab0eee71"</ETag>
    <Size>11</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <Contents>
    <Key>Tom &amp; Jerry.jpg</Key>
    <LastModified>2014-11-21T19:40:05.000Z</LastModified>
    <ETag>"becf17f89c30367a9a44495d62ed521a-1"</ETag>
    <Size>4192256</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <CommonPrefixes>
    <Prefix>photos/</Prefix>
  </CommonPrefixes>
</ListBucketResult>)";

constexpr std::string_view kListUploads = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListMultipartUploadsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>bucket</Bucket>
  <KeyMarker></KeyMarker>
  <UploadIdMarker/>
  <NextKeyMarker>my-movie.m2ts</NextKeyMarker>
  <NextUploadIdMarker>YW55IGlkZWEgd2h5IGVsdmluZydzIHVwbG9hZCBmYWlsZWQ</NextUploadIdMarker>
  <MaxUploads>2</MaxUploads>
  <IsTruncated>true</IsTruncated>
  <Upload>
    <Key>my-divisor</Key>
    <UploadId>XMgbGlrZSBlbHZpbmcncyBub3QgaGF2aW5nIG11Y2ggbHVjaw</UploadId>
    <Initiator>
      <ID>arn:aws:iam::111122223333:user/user1-11111a31-17b5-4fb7-9df5-b111111f13de</ID>
      <DisplayName>user1-11111a31-17b5-4fb7-9df5-b111111f13de</DisplayName>
    </Initiator>
    <StorageClass>REDUCED_REDUNDANCY</StorageClass>
    <Initiated>2010-11-10T20:48:33.000Z</Initiated>
  </Upload>
  <Upload>
    <Key>my-movie.m2ts</Key>
    <UploadId>VXBsb2FkIElEIGZvciBlbHZpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA</UploadId>
    <StorageClass>STANDARD</StorageClass>
    <Initiated>2010-11-10T20:48:33.123456789Z</Initiated>
  </Upload>
</ListMultipartUploadsResult>)";

std::string list_parts_with(std::string_view part) {
    return std::string(R"(<ListPartsResult><IsTruncated>false</IsTruncated><Part>)") +
           std::string(part) + "</Part></ListPartsResult>";
}

std::optional<XmlError> xml_error(const ResponseError& e) {
    if (const auto* x = std::get_if<XmlError>(&e)) {
        return *x;
    }
    return std::nullopt;
}

template <typename T>
std::optional<XmlError> parse_failure(const std::expected<T, ResponseError>& r) {
    return r ? std::nullopt : xml_error(r.error());
}

TEST(S3Xml, ReadsTheUploadIdOfANewMultipartUpload) {
    const auto r = parse_initiate_multipart_upload(kInitiate);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->upload_id, "VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA");
}

TEST(S3Xml, ReadsListedPartsAndKeepsEtagQuotesAsSent) {
    const auto r = parse_list_parts(kListParts);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_truncated);
    EXPECT_EQ(r->next_part_number_marker, 3U);
    ASSERT_EQ(r->parts.size(), 2U);
    EXPECT_EQ(r->parts[0].part_number, 2U);
    EXPECT_EQ(r->parts[0].etag, "\"7778aef83f66abc1fa1e8477f296d394\"");
    EXPECT_EQ(r->parts[0].size, 10'485'760U);
    EXPECT_EQ(r->parts[1].part_number, 3U);
    EXPECT_EQ(r->parts[1].etag, "\"aaaa18db4cc2f85cedef654fccc4a4x8\"");
}

TEST(S3Xml, ATruncatedPartListingMustSayWhereToResume) {
    constexpr std::string_view body =
        "<ListPartsResult><IsTruncated>true</IsTruncated></ListPartsResult>";
    EXPECT_EQ(parse_failure(parse_list_parts(body)), XmlError::MissingElement);

    constexpr std::string_view last_page =
        "<ListPartsResult><IsTruncated>false</IsTruncated></ListPartsResult>";
    const auto r = parse_list_parts(last_page);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->parts.empty());
    EXPECT_EQ(r->next_part_number_marker, std::nullopt);
}

TEST(S3Xml, ReadsTheEtagOfACompletedUpload) {
    const auto r = parse_complete_multipart_upload(kComplete);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->etag, "\"3858f62230ac3c915f300c664312c11f-9\"");
}

TEST(S3Xml, ReportsAnErrorDocumentThatArrivedWithASuccessStatus) {
    const auto r = parse_complete_multipart_upload(kInternalError);
    ASSERT_FALSE(r.has_value());
    const auto* doc = std::get_if<ErrorDocument>(&r.error());
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->code, "InternalError");
    EXPECT_EQ(doc->message, "We encountered an internal error. Please try again.");

    EXPECT_TRUE(std::holds_alternative<ErrorDocument>(
        parse_initiate_multipart_upload(kInternalError).error_or(XmlError::Malformed)));
    EXPECT_TRUE(std::holds_alternative<ErrorDocument>(
        parse_list_parts(kInternalError).error_or(XmlError::Malformed)));
    EXPECT_TRUE(std::holds_alternative<ErrorDocument>(
        parse_list_objects_v2(kInternalError).error_or(XmlError::Malformed)));
    EXPECT_TRUE(std::holds_alternative<ErrorDocument>(
        parse_list_multipart_uploads(kInternalError).error_or(XmlError::Malformed)));
}

TEST(S3Xml, ReadsAnErrorDocument) {
    const auto e = parse_error_document(
        "<Error><Code>NoSuchKey</Code><Message>The resource &lt;key&gt; does not "
        "exist.</Message><Key>a&amp;b</Key></Error>");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->code, "NoSuchKey");
    EXPECT_EQ(e->message, "The resource <key> does not exist.");

    const auto bare = parse_error_document("<Error><Code>SlowDown</Code></Error>");
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(bare->message, "");

    EXPECT_EQ(parse_error_document("<Error><Message>m</Message></Error>"),
              std::unexpected(XmlError::MissingElement));
    EXPECT_EQ(parse_error_document("<Error><Code></Code></Error>"),
              std::unexpected(XmlError::InvalidValue));
    EXPECT_EQ(parse_error_document(kComplete), std::unexpected(XmlError::UnexpectedRoot));
}

TEST(S3Xml, RejectsADocumentOfTheWrongKind) {
    EXPECT_EQ(parse_failure(parse_list_parts(kInitiate)), XmlError::UnexpectedRoot);
    EXPECT_EQ(parse_failure(parse_complete_multipart_upload(kListParts)), XmlError::UnexpectedRoot);
}

TEST(S3Xml, ReadsObjectKeysAndTheContinuationToken) {
    const auto r = parse_list_objects_v2(kListObjects);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_truncated);
    EXPECT_EQ(r->next_continuation_token, "1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=");
    ASSERT_EQ(r->keys.size(), 2U);
    EXPECT_EQ(r->keys[0], "happyface.jpg");
    EXPECT_EQ(r->keys[1], "Tom & Jerry.jpg");

    EXPECT_EQ(parse_failure(parse_list_objects_v2(
                  "<ListBucketResult><IsTruncated>true</IsTruncated></ListBucketResult>")),
              XmlError::MissingElement);
    EXPECT_EQ(
        parse_failure(parse_list_objects_v2("<ListBucketResult><IsTruncated>true</IsTruncated>"
                                            "<NextContinuationToken/></ListBucketResult>")),
        XmlError::MissingElement);
}

TEST(S3Xml, ReadsMultipartUploadsWithTheirStartTimes) {
    const auto r = parse_list_multipart_uploads(kListUploads);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_truncated);
    EXPECT_EQ(r->next_key_marker, "my-movie.m2ts");
    EXPECT_EQ(r->next_upload_id_marker, "YW55IGlkZWEgd2h5IGVsdmluZydzIHVwbG9hZCBmYWlsZWQ");
    ASSERT_EQ(r->uploads.size(), 2U);
    EXPECT_EQ(r->uploads[0].key, "my-divisor");
    EXPECT_EQ(r->uploads[0].upload_id, "XMgbGlrZSBlbHZpbmcncyBub3QgaGF2aW5nIG11Y2ggbHVjaw");

    // 2010-11-10T20:48:33Z is 1289422113 seconds after the epoch.
    const core::WallTime initiated{std::chrono::seconds(1'289'422'113)};
    EXPECT_EQ(r->uploads[0].initiated, initiated);
    EXPECT_EQ(r->uploads[1].initiated, initiated + std::chrono::nanoseconds(123'456'789));
}

std::string upload_initiated_at(std::string_view t) {
    return "<ListMultipartUploadsResult><IsTruncated>false</IsTruncated>"
           "<Upload><Key>k</Key><UploadId>u</UploadId><Initiated>" +
           std::string(t) + "</Initiated></Upload></ListMultipartUploadsResult>";
}

TEST(S3Xml, RejectsStartTimesThatAreNotUtcTimestamps) {
    for (const std::string_view t :
         {"2010-11-10T20:48:33", "2010-11-10T20:48:33+01:00", "2010-13-10T20:48:33.000Z",
          "2010-02-30T20:48:33Z", "2010-11-10T24:00:00Z", "2010-11-10T20:60:33Z",
          "2010-11-10 20:48:33Z", "2010-11-10T20:48:33.Z", "2010-11-10T20:48:33.1234567890Z",
          "2010-11-10T20:48:33,000Z", "10-11-10T20:48:33Z", "", "yesterday"}) {
        EXPECT_EQ(parse_failure(parse_list_multipart_uploads(upload_initiated_at(t))),
                  XmlError::InvalidValue)
            << t;
    }
}

// The edges below are those of int64 nanoseconds, which run from 1677-09-21 to 2262-04-11.
static_assert(std::is_same_v<core::WallTime::duration, std::chrono::nanoseconds>);

TEST(S3Xml, AcceptsStartTimesInTheFirstAndLastWholeYearsAWallTimeHolds) {
    using namespace std::chrono_literals;
    const auto first = parse_list_multipart_uploads(upload_initiated_at("1678-01-01T00:00:00Z"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->uploads.at(0).initiated,
              core::WallTime{std::chrono::sys_days{std::chrono::year{1678} / 1 / 1}});

    const auto last =
        parse_list_multipart_uploads(upload_initiated_at("2261-12-31T23:59:59.999999999Z"));
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->uploads.at(0).initiated,
              core::WallTime{std::chrono::sys_days{std::chrono::year{2262} / 1 / 1}} - 1ns);
}

TEST(S3Xml, RejectsStartTimesOutsideTheWholeYearsAWallTimeHolds) {
    for (const std::string_view t :
         {"1677-12-31T23:59:59.999999999Z", "2262-01-01T00:00:00Z", "2262-04-12T00:00:00Z",
          "9999-12-31T23:59:59Z", "0112-06-15T12:00:00Z", "0000-01-01T00:00:00Z"}) {
        EXPECT_EQ(parse_failure(parse_list_multipart_uploads(upload_initiated_at(t))),
                  XmlError::InvalidValue)
            << t;
    }
}

TEST(S3Xml, DecodesPredefinedAndNumericEntities) {
    const auto r = parse_initiate_multipart_upload(
        "<InitiateMultipartUploadResult><UploadId>&amp;&lt;&gt;&quot;&apos;&#65;&#x42;&#xe9;"
        "&#x1F600;&#8364;</UploadId></InitiateMultipartUploadResult>");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->upload_id, "&<>\"'AB\xc3\xa9\xf0\x9f\x98\x80\xe2\x82\xac");
}

// Markup errors are caught by the shared tree parser, so the shortest document exercises them.
TEST(S3Xml, RejectsMalformedMarkup) {
    for (const std::string_view body : {
             "",
             "   ",
             "<Error>",
             "<Error><Code>c</Code>",
             "<Error><Code>c</Codes></Error>",
             "<Error><Code>c</Error>",
             "<Error/><Error/>",
             "<Error/>trailing",
             "leading<Error/>",
             "<!DOCTYPE x [<!ENTITY a 'b'>]><Error/>",
             "<Error><!-- c --></Error>",
             "<Error><Code><![CDATA[c]]></Code></Error>",
             "<?xml-stylesheet href='a'?><Error/>",
             "<Error><?pi?></Error>",
             " <?xml version='1.0'?><Error/>",
             "<?xml version='1.0'<Error/>",
             "<Error>text<Code>c</Code></Error>",
             "<Error><Code>c</Code>text</Error>",
             "<Error xmlns=unquoted/>",
             "<Error xmlns='a/>",
             "<Error xmlns='a'xmlns:b='c'/>",
             "<Error a='<'/>",
             "<1Error/>",
             "< Error/>",
         }) {
        EXPECT_EQ(parse_error_document(body), std::unexpected(XmlError::Malformed)) << body;
    }
}

TEST(S3Xml, AcceptsTheMarkupS3ActuallySends) {
    for (const std::string_view body : {
             "<Error><Code>c</Code></Error>",
             "<?xml version='1.0' encoding='UTF-8'?>\r\n<Error>\r\n<Code>c</Code>\r\n</Error>\n",
             "<Error xmlns='http://s3.amazonaws.com/doc/2006-03-01/' a = \"b\" ><Code >c</Code "
             "><Key/></Error>",
         }) {
        const auto doc = parse_error_document(body);
        ASSERT_TRUE(doc.has_value()) << body;
        EXPECT_EQ(doc->code, "c") << body;
    }
}

TEST(S3Xml, RejectsBadCharacterData) {
    for (const std::string_view upload_id : std::initializer_list<std::string_view>{
             "a&b", "a&amp", "&nbsp;", "&;", "&#;", "&#x;", "&#0;", "&#x0;", "&#1;", "&#xD800;",
             "&#x110000;", "&#xFFFE;", "&#X41;", "&#-65;", "&#65", "&#x0000000041;", "tab\x01",
             std::string_view("nul\0", 4)}) {
        const std::string body = "<InitiateMultipartUploadResult><UploadId>" +
                                 std::string(upload_id) +
                                 "</UploadId></InitiateMultipartUploadResult>";
        EXPECT_EQ(parse_failure(parse_initiate_multipart_upload(body)), XmlError::Malformed)
            << upload_id;
    }
}

TEST(S3Xml, RefusesNestingBeyondSixteenLevels) {
    const auto nested = [](int levels) {
        std::string body = "<InitiateMultipartUploadResult><UploadId>u</UploadId>";
        for (int i = 1; i < levels; ++i) {
            body += "<a>";
        }
        for (int i = 1; i < levels; ++i) {
            body += "</a>";
        }
        return body + "</InitiateMultipartUploadResult>";
    };
    EXPECT_TRUE(parse_initiate_multipart_upload(nested(16)).has_value());
    EXPECT_EQ(parse_failure(parse_initiate_multipart_upload(nested(17))), XmlError::Malformed);
}

TEST(S3Xml, RejectsMissingDuplicatedAndEmptyFields) {
    EXPECT_EQ(parse_failure(parse_initiate_multipart_upload(
                  "<InitiateMultipartUploadResult><Key>k</Key></InitiateMultipartUploadResult>")),
              XmlError::MissingElement);
    EXPECT_EQ(parse_failure(parse_initiate_multipart_upload(
                  "<InitiateMultipartUploadResult><UploadId>a</UploadId><UploadId>b</UploadId>"
                  "</InitiateMultipartUploadResult>")),
              XmlError::DuplicateElement);
    EXPECT_EQ(parse_failure(parse_initiate_multipart_upload(
                  "<InitiateMultipartUploadResult><UploadId/></InitiateMultipartUploadResult>")),
              XmlError::InvalidValue);
    EXPECT_EQ(parse_failure(parse_initiate_multipart_upload(
                  "<InitiateMultipartUploadResult><UploadId><x/></UploadId>"
                  "</InitiateMultipartUploadResult>")),
              XmlError::InvalidValue);
    EXPECT_EQ(parse_failure(parse_list_parts(list_parts_with(
                  "<PartNumber>1</PartNumber><PartNumber>2</PartNumber><ETag>e</ETag>"
                  "<Size>1</Size>"))),
              XmlError::DuplicateElement);
    EXPECT_EQ(parse_failure(
                  parse_list_parts(list_parts_with("<PartNumber>1</PartNumber><Size>1</Size>"))),
              XmlError::MissingElement);
}

std::expected<infra::s3util::ListPartsResult, ResponseError> one_part(std::string_view number,
                                                                      std::string_view size) {
    return parse_list_parts(list_parts_with("<PartNumber>" + std::string(number) +
                                            "</PartNumber><ETag>'e'</ETag><Size>" +
                                            std::string(size) + "</Size>"));
}

TEST(S3Xml, AcceptsNumbersUpToTheirLimits) {
    const auto largest = one_part("10000", "18446744073709551615");
    ASSERT_TRUE(largest.has_value());
    ASSERT_EQ(largest->parts.size(), 1U);
    EXPECT_EQ(largest->parts.front().part_number, 10'000U);
    EXPECT_EQ(largest->parts.front().size, UINT64_MAX);
}

TEST(S3Xml, RejectsPartNumbersOutsideOneToTenThousand) {
    for (const std::string_view bad :
         {"0", "10001", "4294967297", "-1", "+1", " 1", "1 ", "0x10", "1e3", "", "one"}) {
        EXPECT_EQ(parse_failure(one_part(bad, "1")), XmlError::InvalidValue) << bad;
    }
}

TEST(S3Xml, RejectsSizesThatAreNotPlainUnsigned64BitNumbers) {
    for (const std::string_view bad : {"18446744073709551616", "-1", "+5", "5.0", ""}) {
        EXPECT_EQ(parse_failure(one_part("1", bad)), XmlError::InvalidValue) << bad;
    }
}

TEST(S3Xml, AcceptsOnlyLowercaseTrueOrFalseAsAFlag) {
    for (const std::string_view flag : {"TRUE", "1", "yes", ""}) {
        const std::string body = "<ListPartsResult><IsTruncated>" + std::string(flag) +
                                 "</IsTruncated></ListPartsResult>";
        EXPECT_EQ(parse_failure(parse_list_parts(body)), XmlError::InvalidValue) << flag;
    }
}

TEST(CompleteMultipartUploadBody, ListsPartsInAscendingOrderWithEscapedEtags) {
    const std::array parts{
        CompletedPart{.part_number = 3, .etag = "\"c3\""},
        CompletedPart{.part_number = 1, .etag = "\"a1\""},
        CompletedPart{.part_number = 2, .etag = "\"b&2<>'\""},
    };
    EXPECT_EQ(
        complete_multipart_upload_body(parts),
        R"(<CompleteMultipartUpload xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)"
        "<Part><PartNumber>1</PartNumber><ETag>&quot;a1&quot;</ETag></Part>"
        "<Part><PartNumber>2</PartNumber><ETag>&quot;b&amp;2&lt;&gt;&apos;&quot;</ETag></Part>"
        "<Part><PartNumber>3</PartNumber><ETag>&quot;c3&quot;</ETag></Part>"
        "</CompleteMultipartUpload>");
}

} // namespace
