#pragma once

#include <cstdint>

namespace http {

// The methods the gateway routes. Everything else llhttp knows (CONNECT, TRACE, the WebDAV
// set) parses fine and arrives as Other, so the answer is a status code, not a parse error.
enum class Method : std::uint8_t { Get, Head, Post, Put, Patch, Delete, Options, Other };

} // namespace http
