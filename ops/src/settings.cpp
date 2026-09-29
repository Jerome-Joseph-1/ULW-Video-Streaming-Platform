#include "ops/settings.hpp"

#include "os/unique_fd.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
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

// A flag's value, attached with '=' or as the next argument, which `i` then moves past.
std::optional<std::string> flag_value(std::span<const std::string_view> args, std::size_t& i) {
    const std::string_view arg = args[i];
    if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
        return std::string(arg.substr(eq + 1));
    }
    if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
        return std::string(args[++i]);
    }
    return std::nullopt;
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
        const std::string_view name = arg.substr(2);
        const std::string_view flag = name.substr(0, name.find('='));
        auto value = flag_value(args, i);
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

std::expected<FileLayer, SettingsError> read_config_file(const std::string& path) {
    // A file of scalars is a few KiB; anything past 64 KiB is not a configuration file.
    constexpr std::size_t kMaxFile = std::size_t{64} * 1024;
    // O_NONBLOCK: opening a FIFO for reading would otherwise wait for a writer that may never
    // come. It changes nothing for the regular file this must be, which is checked next.
    const os::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    if (!fd) {
        return error(path, std::generic_category().message(errno));
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return error(path, std::generic_category().message(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return error(path, "not a regular file");
    }
    // Any setting is enough to take the service over (a key set, a JWKS URL, a TLS key), so a
    // file someone else could have written is not read at all.
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return error(path, "writable by others than its owner; make it 0644 or stricter");
    }
    if (st.st_uid != 0 && st.st_uid != ::geteuid()) {
        return error(path, "owned by neither root nor the user the service runs as");
    }
    std::string text;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return error(path, std::generic_category().message(errno));
        }
        if (n == 0) {
            break;
        }
        text.append(buf.data(), static_cast<std::size_t>(n));
        if (text.size() > kMaxFile) {
            return error(path, "larger than 64 KiB");
        }
    }
    auto entries = toml::parse(text);
    if (!entries) {
        return error(path + ":" + std::to_string(entries.error().line), entries.error().reason);
    }
    return FileLayer{.path = path,
                     .entries = std::move(*entries),
                     .private_to_owner = (st.st_mode & (S_IRWXG | S_IRWXO)) == 0};
}

std::expected<Settings, SettingsError> load_settings(std::span<const Setting> schema,
                                                     const CommandLine& cli, const Lookup& env) {
    std::optional<std::string> path = cli.config_file;
    if (!path) {
        path = env("ULW_CONFIG");
    }
    if (!path || path->empty()) {
        return Settings::layer(schema, nullptr, env, cli);
    }
    auto file = read_config_file(*path);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    return Settings::layer(schema, &*file, env, cli);
}

} // namespace ops
