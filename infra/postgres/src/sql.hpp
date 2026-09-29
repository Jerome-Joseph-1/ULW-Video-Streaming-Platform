#pragma once

namespace infra::postgres {

// The text of a statement. Only a compile-time constant converts to one, so data reaches the
// server as a bound parameter and never as part of the statement text.
class Sql {
public:
    // Implicit, so a literal can stand wherever a statement is expected.
    consteval Sql(const char* text) noexcept : text_(text) {}

    [[nodiscard]] const char* c_str() const noexcept { return text_; }

private:
    const char* text_;
};

} // namespace infra::postgres
