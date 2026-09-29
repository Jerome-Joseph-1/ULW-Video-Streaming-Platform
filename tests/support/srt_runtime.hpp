#pragma once

#include "infra/srt/ingest.hpp"

#include <gtest/gtest.h>
#include <optional>

namespace ulw::test {

// libsrt started for the whole test run and cleaned up before gtest returns, so its worker
// threads have ended, and released their OpenSSL state, by the time a leak check looks.
class SrtRuntimeEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        auto started = infra::srt::Runtime::start();
        ASSERT_TRUE(started) << started.error();
        runtime_.emplace(std::move(*started));
    }
    void TearDown() override { runtime_.reset(); }

    [[nodiscard]] const infra::srt::Runtime& runtime() const { return *runtime_; }

private:
    std::optional<infra::srt::Runtime> runtime_;
};

// Registered when the test binary starts; gtest owns the environment.
inline SrtRuntimeEnvironment* const kSrtRuntime = new SrtRuntimeEnvironment;
inline const ::testing::Environment* const kSrtRuntimeRegistered =
    ::testing::AddGlobalTestEnvironment(kSrtRuntime);

} // namespace ulw::test
