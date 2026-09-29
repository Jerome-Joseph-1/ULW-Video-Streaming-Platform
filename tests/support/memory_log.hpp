#pragma once

#include "ops/log.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ulw::test {

// Keeps every line, without its newline, for tests to inspect. Thread-safe.
class MemoryLog final : public ops::ILogSink {
public:
    void write(std::string_view line) noexcept override {
        if (line.ends_with('\n')) {
            line.remove_suffix(1);
        }
        const std::scoped_lock lock(mutex_);
        lines_.emplace_back(line);
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept override { return 0; }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::scoped_lock lock(mutex_);
        return lines_;
    }
    [[nodiscard]] std::string all() const {
        std::string out;
        for (const std::string& l : lines()) {
            out += l;
            out += '\n';
        }
        return out;
    }
    // Lines whose "event" is `event`.
    [[nodiscard]] std::vector<std::string> events(std::string_view event) const {
        const std::string needle = R"("event":")" + std::string(event) + '"';
        std::vector<std::string> out;
        for (const std::string& l : lines()) {
            if (l.find(needle) != std::string::npos) {
                out.push_back(l);
            }
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

} // namespace ulw::test
