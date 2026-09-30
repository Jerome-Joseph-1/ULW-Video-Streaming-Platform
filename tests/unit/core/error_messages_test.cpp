#include "core/errors/domain_error.hpp"
#include "core/ports/media.hpp"
#include "core/ports/storage.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <string_view>
#include <utility>

namespace {

using core::DomainError;
using core::ports::MediaError;
using core::ports::StorageError;

// Walks every value up to `last` and then one past it: the fallback message there proves `last`
// really is the final enumerator, so a new one cannot slip in untested.
template <typename Enum> void expect_distinct_messages(Enum last, std::string_view fallback) {
    std::set<std::string_view> seen;
    for (unsigned i = 0; i <= std::to_underlying(last); ++i) {
        const std::string_view message = to_string(static_cast<Enum>(i));
        EXPECT_FALSE(message.empty()) << i;
        EXPECT_NE(message, fallback) << i;
        EXPECT_TRUE(seen.insert(message).second) << "duplicate: " << message;
    }
    const auto past_last = static_cast<std::uint8_t>(std::to_underlying(last) + 1);
    EXPECT_EQ(to_string(static_cast<Enum>(past_last)), fallback);
}

TEST(DomainError, EveryEnumeratorHasItsOwnMessage) {
    expect_distinct_messages(DomainError::InvalidFailureReason, "unknown domain error");
}

TEST(StorageError, EveryEnumeratorHasItsOwnMessage) {
    expect_distinct_messages(StorageError::Corrupt, "unknown storage error");
}

TEST(MediaError, EveryEnumeratorHasItsOwnMessage) {
    expect_distinct_messages(MediaError::NotImplemented, "unknown media error");
}

} // namespace
