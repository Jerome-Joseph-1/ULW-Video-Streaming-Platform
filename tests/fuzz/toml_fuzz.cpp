// Feeds arbitrary bytes to the configuration file parser. Whatever it accepts must survive a
// round trip: the entries written back out as TOML parse to the same entries, so nothing the
// parser returns depends on how the file happened to spell it.

#include "ops/toml.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

void check(bool invariant) {
    if (!invariant) {
        __builtin_trap();
    }
}

void quote(std::string& out, std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdef";
    out += '"';
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20 || u == 0x7f) {
            out += "\\u00";
            out += kHex[u >> 4U];
            out += kHex[u & 0xFU];
        } else {
            out += c;
        }
    }
    out += '"';
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The fuzzer hands over bytes; the parser takes text.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto parsed = ops::toml::parse(input);
    if (!parsed) {
        check(!parsed.error().reason.empty());
        return 0;
    }
    std::string again;
    for (const ops::toml::Entry& e : *parsed) {
        again += e.key;
        again += " = ";
        if (e.kind == ops::toml::Kind::String) {
            quote(again, e.value);
        } else {
            again += e.value;
        }
        again += '\n';
    }
    const auto reparsed = ops::toml::parse(again);
    check(reparsed.has_value());
    check(reparsed->size() == parsed->size());
    for (std::size_t i = 0; i < parsed->size(); ++i) {
        check((*reparsed)[i].key == (*parsed)[i].key);
        check((*reparsed)[i].kind == (*parsed)[i].kind);
        check((*reparsed)[i].value == (*parsed)[i].value);
    }
    return 0;
}
