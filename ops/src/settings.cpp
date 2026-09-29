#include "ops/settings.hpp"

#include <algorithm>
#include <utility>

namespace ops {

namespace {

std::string flag_of(std::string_view key) {
    std::string flag(key);
    std::ranges::replace(flag, '.', '-');
    std::ranges::replace(flag, '_', '-');
    return flag;
}

std::unexpected<SettingsError> error(std::string source, std::string_view reason) {
    return std::unexpected(
        SettingsError{.source = std::move(source), .reason = std::string(reason)});
}

const Setting* by_flag(std::span<const Setting> schema, std::string_view flag) {
    const auto it = std::ranges::find_if(
        schema, [&](const Setting& s) { return !s.key.empty() && flag_of(s.key) == flag; });
    return it == schema.end() ? nullptr : &*it;
}

} // namespace

std::string_view to_string(Origin origin) noexcept {
    switch (origin) {
    case Origin::Default:
        return "default";
    case Origin::File:
        return "file";
    case Origin::Environment:
        return "env";
    case Origin::CommandLine:
        return "cli";
    }
    return "default";
}

std::expected<CommandLine, SettingsError>
parse_command_line(std::span<const Setting> schema, std::span<const std::string_view> args) {
    CommandLine cli;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!arg.starts_with("--")) {
            return error(std::string(arg), "unexpected argument");
        }
        if (arg == "--version") {
            cli.version = true;
            continue;
        }
        if (arg == "--check-config") {
            cli.check = true;
            continue;
        }
        const std::size_t eq = arg.find('=');
        const std::string_view flag =
            arg.substr(2, eq == std::string_view::npos ? arg.npos : eq - 2);
        std::optional<std::string> value;
        if (eq != std::string_view::npos) {
            value = std::string(arg.substr(eq + 1));
        } else if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
            value = std::string(args[++i]);
        }
        if (!value) {
            return error(std::string(arg), "needs a value");
        }
        if (flag == "config") {
            if (cli.config_file) {
                return error("--config", "given twice");
            }
            cli.config_file = std::move(value);
            continue;
        }
        const Setting* setting = by_flag(schema, flag);
        if (setting == nullptr) {
            return error("--" + std::string(flag), "unknown option");
        }
        if (setting->secret) {
            return error("--" + std::string(flag),
                         "is a secret; set it in the environment or a file only its owner reads");
        }
        if (!cli.values.emplace(std::string(setting->env), std::move(*value)).second) {
            return error("--" + std::string(flag), "given twice");
        }
    }
    return cli;
}

std::expected<Settings, SettingsError> Settings::layer(std::span<const Setting> schema,
                                                       const FileLayer* file, const Lookup& env,
                                                       const CommandLine& cli) {
    Settings out;
    if (file != nullptr) {
        for (const toml::Entry& e : file->entries) {
            const auto it = std::ranges::find_if(
                schema, [&](const Setting& s) { return !s.key.empty() && s.key == e.key; });
            const std::string where = file->path + ":" + std::to_string(e.line) + ": " + e.key;
            if (it == schema.end()) {
                return error(where, "unknown setting");
            }
            if (it->secret && !file->private_to_owner) {
                return error(where, "is a secret, but other users may read the file; make it "
                                    "0400 or 0600");
            }
            out.values_.insert_or_assign(std::string(it->env),
                                         Value{.text = e.value, .origin = Origin::File});
        }
    }
    for (const Setting& s : schema) {
        auto value = env(s.env);
        if (value && !value->empty()) {
            out.values_.insert_or_assign(std::string(s.env), Value{.text = std::move(*value),
                                                                   .origin = Origin::Environment});
        }
    }
    for (const auto& [name, value] : cli.values) {
        out.values_.insert_or_assign(name, Value{.text = value, .origin = Origin::CommandLine});
    }
    return out;
}

std::optional<std::string> Settings::get(std::string_view env) const {
    const auto it = values_.find(env);
    if (it == values_.end() || it->second.text.empty()) {
        return std::nullopt;
    }
    return it->second.text;
}

Origin Settings::origin(std::string_view env) const {
    const auto it = values_.find(env);
    return it == values_.end() ? Origin::Default : it->second.origin;
}

Lookup Settings::lookup() const {
    return [values = values_](std::string_view env) -> std::optional<std::string> {
        const auto it = values.find(env);
        if (it == values.end() || it->second.text.empty()) {
            return std::nullopt;
        }
        return it->second.text;
    };
}

} // namespace ops
