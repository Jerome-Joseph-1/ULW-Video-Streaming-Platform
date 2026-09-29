#pragma once

#include <string>

namespace infra::postgres {

// Whether libpq would read `conninfo`, a URL or keyword=value pairs, without connecting. For
// configuration checks at startup, so a string that cannot work stops the process before
// anything is started.
[[nodiscard]] bool connection_string_parses(const std::string& conninfo);

} // namespace infra::postgres
