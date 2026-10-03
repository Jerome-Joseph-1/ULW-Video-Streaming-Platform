#include "scratch.hpp"

#include <system_error>

namespace live {

namespace fs = std::filesystem;

std::expected<void, std::string> clear_scratch(const fs::path& scratch) {
    // The scratch root is the deployment's to make, owned by the packager's user and 0700. Made
    // here, it would be made by a process that cannot write to /var/cache, or in a directory
    // where someone else could have made it first.
    std::error_code ec;
    const fs::path root = scratch.parent_path();
    if (!fs::is_directory(root, ec)) {
        // Missing is the usual case and gets the instructions; anything else (EACCES) is told.
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return std::unexpected(root.string() + ": " + ec.message());
        }
        return std::unexpected(root.string() +
                               " is not a directory: make it, owned by the user the packager "
                               "runs as with mode 0700, or name another");
    }
    // What an earlier run left is not needed: the store has the stream's state.
    fs::remove_all(scratch, ec);
    if (ec) {
        return std::unexpected(scratch.string() + ": " + ec.message());
    }
    // One level at a time: a root that went away since the check fails here, rather than being
    // made again by the packager.
    fs::create_directory(scratch, ec);
    if (!ec) {
        fs::create_directory(scratch / "media", ec);
    }
    if (ec) {
        return std::unexpected(scratch.string() + ": " + ec.message());
    }
    return {};
}

} // namespace live
