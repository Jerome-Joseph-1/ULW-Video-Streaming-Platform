#pragma once

#include <cstdint>
#include <initializer_list>

namespace http {

// The methods the gateway routes. Everything else llhttp knows (TRACE, the WebDAV set) parses
// fine and arrives as Other, so the answer is a status code, not a parse error. CONNECT never
// arrives: the parser refuses it, because llhttp would treat what follows as a tunnel.
enum class Method : std::uint8_t { Get, Head, Post, Put, Patch, Delete, Options, Other };

class MethodSet {
public:
    constexpr MethodSet() noexcept = default;
    constexpr MethodSet(std::initializer_list<Method> methods) noexcept {
        for (const Method m : methods) {
            add(m);
        }
    }

    constexpr void add(Method m) noexcept { bits_ = static_cast<std::uint8_t>(bits_ | bit(m)); }
    [[nodiscard]] constexpr bool contains(Method m) const noexcept { return (bits_ & bit(m)) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

    friend constexpr bool operator==(MethodSet, MethodSet) noexcept = default;

private:
    static constexpr unsigned bit(Method m) noexcept { return 1U << static_cast<unsigned>(m); }

    std::uint8_t bits_ = 0;
};

} // namespace http
