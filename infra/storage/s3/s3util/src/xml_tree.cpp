#include "xml_tree.hpp"

#include "infra/s3util/xml.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace infra::s3util::detail {

namespace {

// The deepest document read here nests three levels (ListPartsResult/Part/ETag); the bound
// only keeps a hostile body from growing the open-element stack without limit.
constexpr std::size_t kMaxDepth = 16;

// "&#x10FFFF;" is ten bytes. Leading zeros could make a valid reference longer, but no S3
// implementation writes them, and the bound keeps a stray '&' from scanning the whole body.
constexpr std::size_t kMaxReferenceLength = 12;

bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_blank_text(std::string_view s) noexcept {
    return s.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

bool is_name_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':';
}

bool is_name_char(char c) noexcept {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

// XML 1.0 Char production.
bool is_xml_char(std::uint32_t c) noexcept {
    return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) ||
           (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
}

struct Reference {
    std::uint32_t code_point;
    std::size_t length;
};

std::optional<std::uint32_t> parse_code_point(std::string_view digits, int base) noexcept {
    std::uint32_t value = 0;
    const char* const end = std::to_address(digits.end());
    const auto [ptr, ec] = std::from_chars(std::to_address(digits.begin()), end, value, base);
    if (digits.empty() || ec != std::errc{} || ptr != end || !is_xml_char(value)) {
        return std::nullopt;
    }
    return value;
}

// `s` starts at an '&'.
std::optional<Reference> parse_reference(std::string_view s) noexcept {
    const std::size_t semicolon = s.substr(0, kMaxReferenceLength).find(';');
    if (semicolon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view body = s.substr(1, semicolon - 1);
    const std::size_t length = semicolon + 1;
    std::optional<std::uint32_t> code_point;
    if (body == "amp") {
        code_point = '&';
    } else if (body == "lt") {
        code_point = '<';
    } else if (body == "gt") {
        code_point = '>';
    } else if (body == "quot") {
        code_point = '"';
    } else if (body == "apos") {
        code_point = '\'';
    } else if (body.starts_with("#x")) {
        code_point = parse_code_point(body.substr(2), 16);
    } else if (body.starts_with('#')) {
        code_point = parse_code_point(body.substr(1), 10);
    }
    if (!code_point) {
        return std::nullopt;
    }
    return Reference{.code_point = *code_point, .length = length};
}

bool is_valid_char_data(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '&') {
            const auto ref = parse_reference(text.substr(i));
            if (!ref) {
                return false;
            }
            i += ref->length;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && !is_blank(c)) {
            return false;
        }
        ++i;
    }
    return true;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view in) noexcept : in_(in) {}

    std::expected<std::vector<XmlNode>, XmlError> run() {
        if (!skip_declaration()) {
            return std::unexpected(XmlError::Malformed);
        }
        while (true) {
            const std::size_t lt = in_.find('<', pos_);
            const std::string_view text = in_.substr(pos_, lt - pos_);
            if (!is_valid_char_data(text)) {
                return std::unexpected(XmlError::Malformed);
            }
            if (lt == std::string_view::npos) {
                if (!open_.empty() || nodes_.empty() || !is_blank_text(text)) {
                    return std::unexpected(XmlError::Malformed);
                }
                return std::move(nodes_);
            }
            pos_ = lt;
            const bool ok =
                in_.substr(pos_).starts_with("</") ? close_element(text) : open_element(text);
            if (!ok) {
                return std::unexpected(XmlError::Malformed);
            }
        }
    }

private:
    struct OpenElement {
        std::uint32_t index;
        std::uint32_t last_child = kNoNode;
    };

    // "<?xml-stylesheet" and every other processing instruction are refused; only the
    // declaration itself is skipped, and only at the very start.
    bool skip_declaration() noexcept {
        if (!in_.starts_with("<?xml")) {
            return true;
        }
        constexpr std::size_t kTagLength = std::string_view("<?xml").size();
        if (in_.size() <= kTagLength || !is_blank(in_[kTagLength])) {
            return false;
        }
        const std::size_t end = in_.find("?>");
        if (end == std::string_view::npos) {
            return false;
        }
        pos_ = end + 2;
        return true;
    }

    bool skip_blanks() noexcept {
        const std::size_t start = pos_;
        while (pos_ < in_.size() && is_blank(in_[pos_])) {
            ++pos_;
        }
        return pos_ != start;
    }

    bool consume(std::string_view token) noexcept {
        if (!in_.substr(pos_).starts_with(token)) {
            return false;
        }
        pos_ += token.size();
        return true;
    }

    std::optional<std::string_view> name() noexcept {
        const std::size_t start = pos_;
        if (pos_ >= in_.size() || !is_name_start(in_[pos_])) {
            return std::nullopt;
        }
        while (pos_ < in_.size() && is_name_char(in_[pos_])) {
            ++pos_;
        }
        return in_.substr(start, pos_ - start);
    }

    // Consumes the rest of a start tag. Attribute values are validated and dropped: the
    // documents read here carry nothing but an xmlns in them.
    std::optional<bool> finish_start_tag() noexcept {
        while (true) {
            const bool spaced = skip_blanks();
            if (consume("/>")) {
                return true;
            }
            if (consume(">")) {
                return false;
            }
            if (!spaced || !name()) {
                return std::nullopt;
            }
            skip_blanks();
            if (!consume("=")) {
                return std::nullopt;
            }
            skip_blanks();
            if (pos_ >= in_.size() || (in_[pos_] != '"' && in_[pos_] != '\'')) {
                return std::nullopt;
            }
            const std::size_t close = in_.find(in_[pos_], pos_ + 1);
            if (close == std::string_view::npos) {
                return std::nullopt;
            }
            const std::string_view value = in_.substr(pos_ + 1, close - pos_ - 1);
            if (value.find('<') != std::string_view::npos || !is_valid_char_data(value)) {
                return std::nullopt;
            }
            pos_ = close + 1;
        }
    }

    bool open_element(std::string_view text_before) {
        // Text before the root, after it, or beside a sibling element is not in any document
        // S3 sends.
        if (!is_blank_text(text_before) || (open_.empty() && !nodes_.empty()) ||
            open_.size() == kMaxDepth || nodes_.size() >= kNoNode) {
            return false;
        }
        ++pos_;
        const auto tag = name();
        if (!tag) {
            return false;
        }
        const auto self_closing = finish_start_tag();
        if (!self_closing) {
            return false;
        }
        const auto index = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(
            XmlNode{.name = *tag, .text = {}, .first_child = kNoNode, .next_sibling = kNoNode});
        if (!open_.empty()) {
            OpenElement& parent = open_.back();
            if (parent.last_child == kNoNode) {
                nodes_[parent.index].first_child = index;
            } else {
                nodes_[parent.last_child].next_sibling = index;
            }
            parent.last_child = index;
        }
        if (!*self_closing) {
            open_.push_back(OpenElement{.index = index});
        }
        return true;
    }

    bool close_element(std::string_view text_before) {
        pos_ += 2;
        const auto tag = name();
        skip_blanks();
        if (!tag || !consume(">") || open_.empty()) {
            return false;
        }
        const OpenElement closing = open_.back();
        XmlNode& node = nodes_[closing.index];
        if (node.name != *tag) {
            return false;
        }
        if (closing.last_child == kNoNode) {
            node.text = text_before;
        } else if (!is_blank_text(text_before)) {
            return false;
        }
        open_.pop_back();
        return true;
    }

    std::string_view in_;
    std::size_t pos_ = 0;
    std::vector<XmlNode> nodes_;
    std::vector<OpenElement> open_;
};

} // namespace

std::expected<XmlDocument, XmlError> XmlDocument::parse(std::string_view text) {
    auto nodes = Parser(text).run();
    if (!nodes) {
        return std::unexpected(nodes.error());
    }
    return XmlDocument(std::move(*nodes));
}

std::expected<const XmlNode*, XmlError> XmlDocument::find(const XmlNode& parent,
                                                          std::string_view name) const {
    const XmlNode* found = nullptr;
    for (const XmlNode* child = first_child(parent); child != nullptr;
         child = next_sibling(*child)) {
        if (child->name != name) {
            continue;
        }
        if (found != nullptr) {
            return std::unexpected(XmlError::DuplicateElement);
        }
        found = child;
    }
    return found;
}

std::string decode_text(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            ++i;
            continue;
        }
        // Validated at parse time, so a reference here always resolves.
        const auto ref =
            parse_reference(raw.substr(i)).value_or(Reference{.code_point = '&', .length = 1});
        append_utf8(out, ref.code_point);
        i += ref.length;
    }
    return out;
}

} // namespace infra::s3util::detail
