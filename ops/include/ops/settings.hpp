#pragma once

#include "ops/toml.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ops {

// One configurable value. The environment variable is its canonical name, the one the
// deployment manifests use; `key` is its name in a configuration file, and `--key-with-dashes`
// (dots and underscores both become dashes) its command-line flag.
struct Setting {
    std::string_view env;
    // Empty: set only through the environment.
    std::string_view key;
    // Never accepted on the command line, which every local user can read through /proc, and
    // only from a file no other user can read. Always logged redacted.
    bool secret = false;
};

enum class Origin : std::uint8_t { Default, File, Environment, CommandLine };

[[nodiscard]] std::string_view to_string(Origin origin) noexcept;

struct SettingsError {
    // The flag, file key or variable at fault.
    std::string source;
    std::string reason;
};

// The variable's value, or nullopt when it is unset. An empty value counts as unset.
using Lookup = std::function<std::optional<std::string>(std::string_view name)>;

struct CommandLine {
    std::optional<std::string> config_file;
    bool version = false;
    // Validate and describe the configuration, then exit.
    bool check = false;
    // By environment variable name.
    std::map<std::string, std::string, std::less<>> values;
};

// `--flag=value` or `--flag value`, plus --config, --check-config and --version.
[[nodiscard]] std::expected<CommandLine, SettingsError>
parse_command_line(std::span<const Setting> schema, std::span<const std::string_view> args);

// A configuration file's entries, and whether anyone but its owner may read it.
struct FileLayer {
    std::string path;
    std::vector<toml::Entry> entries;
    bool private_to_owner = false;
};

// Defaults < file < environment < command line. The defaults live with the code that reads
// the values: what this yields is a lookup by variable name for it.
class Settings {
public:
    [[nodiscard]] static std::expected<Settings, SettingsError>
    layer(std::span<const Setting> schema, const FileLayer* file, const Lookup& env,
          const CommandLine& cli);

    [[nodiscard]] std::optional<std::string> get(std::string_view env) const;
    [[nodiscard]] Origin origin(std::string_view env) const;
    [[nodiscard]] Lookup lookup() const;

private:
    struct Value {
        std::string text;
        Origin origin;
    };
    std::map<std::string, Value, std::less<>> values_;
};

// Reads, parses and checks the file at `path`: owner-only means no permission bits for group
// or others.
[[nodiscard]] std::expected<FileLayer, SettingsError> read_config_file(const std::string& path);

// All the layers: the file --config names, or else ULW_CONFIG, if either does.
[[nodiscard]] std::expected<Settings, SettingsError>
load_settings(std::span<const Setting> schema, const CommandLine& cli, const Lookup& env);

} // namespace ops
