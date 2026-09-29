#pragma once

// Clang warns when a call's result outlives a temporary passed as an argument marked with this.
// GCC 14 has no such attribute and warns about an unknown one, so there it expands to nothing.
#if defined(__clang__) && __has_cpp_attribute(clang::lifetimebound)
#define ULW_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define ULW_LIFETIMEBOUND
#endif
