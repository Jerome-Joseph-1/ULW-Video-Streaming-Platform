#pragma once

#include <chrono>

namespace core {

using Millis = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
using MonoTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;

} // namespace core
