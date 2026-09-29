// Reads a session description on stdin, parses and validates it, and writes it back
// serialized. On a parse error it prints "line N: <message>" to stderr and exits 1.
// tests/e2e/sdp_roundtrip.spec.mjs runs Chromium's offers and answers through it.

#include "codec/sdp/error.hpp"
#include "codec/sdp/parser.hpp"
#include "codec/sdp/serializer.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>

int main() {
    std::string text;
    std::array<char, 4096> chunk{};
    while (std::cin.read(chunk.data(), chunk.size()) || std::cin.gcount() > 0) {
        text.append(chunk.data(), static_cast<std::size_t>(std::cin.gcount()));
    }
    const auto session = codec::sdp::parse(text);
    if (!session) {
        std::cerr << "line " << session.error().line << ": "
                  << codec::sdp::message(session.error().code) << '\n';
        return 1;
    }
    std::cout << codec::sdp::serialize(*session) << std::flush;
    return 0;
}
