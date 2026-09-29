#pragma once

#include "core/ports/message_store.hpp"
#include "net/reactor.hpp"

#include "support/reactor_harness.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <utility>

namespace ulw::test {

// Pumps the loop until the store answers; a store that never answers fails the test.
template <class T, class Call>
core::ports::MessageResult<T> ask(net::IReactor& reactor, Call call) {
    std::optional<core::ports::MessageResult<T>> answer;
    call([&answer](core::ports::MessageResult<T> r) noexcept { answer = std::move(r); });
    if (!pump_until(reactor, [&] { return answer.has_value(); })) {
        ADD_FAILURE() << "the message store never answered";
        return std::unexpected(core::ports::MessageStoreError::Unavailable);
    }
    return std::move(*answer);
}

} // namespace ulw::test
