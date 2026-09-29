#include "stream_id.hpp"

#include <algorithm>

namespace live {

std::expected<StreamId, std::string> StreamId::parse(std::string_view text) {
    if (text.empty() || text.size() > kMaxLength) {
        return std::unexpected("not 1 to 64 characters");
    }
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    };
    if (!std::ranges::all_of(text, allowed)) {
        return std::unexpected("only letters, digits, '_' and '-' are allowed");
    }
    return StreamId(std::string(text));
}

} // namespace live
