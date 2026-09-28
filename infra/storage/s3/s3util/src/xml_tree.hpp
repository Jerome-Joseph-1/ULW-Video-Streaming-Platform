#pragma once

#include "infra/s3util/xml.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace infra::s3util::detail {

inline constexpr std::uint32_t kNoNode = std::numeric_limits<std::uint32_t>::max();

struct XmlNode {
    std::string_view name;
    // Character data of an element without children, entities still encoded but already
    // validated. Empty for an element with children.
    std::string_view text;
    std::uint32_t first_child = kNoNode;
    std::uint32_t next_sibling = kNoNode;
};

// The strict subset of XML 1.0 that S3-compatible services emit: an optional declaration, one
// root element, attributes (skipped), character data and the five predefined and numeric
// entity references. DOCTYPE, comments, CDATA, processing instructions and mixed content are
// refused, which also rules out every entity-expansion attack. Views into the document stay
// valid only while the document text does.
class XmlDocument {
public:
    [[nodiscard]] static std::expected<XmlDocument, XmlError> parse(std::string_view text);

    [[nodiscard]] const XmlNode& root() const noexcept { return nodes_.front(); }
    [[nodiscard]] const XmlNode* first_child(const XmlNode& parent) const noexcept {
        return at(parent.first_child);
    }
    [[nodiscard]] const XmlNode* next_sibling(const XmlNode& node) const noexcept {
        return at(node.next_sibling);
    }

    // The only child called `name`: nullptr when there is none, DuplicateElement when there
    // are several.
    [[nodiscard]] std::expected<const XmlNode*, XmlError> find(const XmlNode& parent,
                                                               std::string_view name) const;

private:
    explicit XmlDocument(std::vector<XmlNode> nodes) : nodes_(std::move(nodes)) {}

    [[nodiscard]] const XmlNode* at(std::uint32_t index) const noexcept {
        return index == kNoNode ? nullptr : &nodes_[index];
    }

    std::vector<XmlNode> nodes_;
};

// Decodes character data that XmlDocument::parse has already validated.
[[nodiscard]] std::string decode_text(std::string_view raw);

} // namespace infra::s3util::detail
