#include "scratch.hpp"

#include "os/private_dir.hpp"

#include <system_error>
#include <utility>
#include <vector>

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
    // Checked before anything in it is touched: a root that is a symbolic link, or another
    // user's, could have the stream's directory swapped for one of theirs.
    if (auto held = os::check_private_parent(root); !held) {
        return std::unexpected(std::move(held.error()));
    }
    // A link, or a file, in the stream's place is taken away; a link is not followed, and what it
    // points at is left.
    const fs::file_status status = fs::symlink_status(scratch, ec);
    if (ec && status.type() != fs::file_type::not_found) {
        return std::unexpected(scratch.string() + ": " + ec.message());
    }
    ec.clear();
    if (fs::exists(status) && !fs::is_directory(status)) {
        fs::remove(scratch, ec);
        if (ec) {
            return std::unexpected(scratch.string() + ": " + ec.message());
        }
    }
    // Owner-only, and refused if it is another user's, before what is in it is removed. Nothing
    // above it is made: a root that went away since the check fails here, rather than being made
    // again by the packager.
    if (auto made = os::make_private_dir(scratch, os::MissingParent::Refuse); !made) {
        return std::unexpected(std::move(made.error()));
    }
    // What an earlier run left is not needed: the store has the stream's state.
    std::vector<fs::path> left;
    for (auto it = fs::directory_iterator(scratch, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        left.push_back(it->path());
    }
    for (const fs::path& entry : left) {
        // remove_all takes a link away without following it.
        if (!ec) {
            fs::remove_all(entry, ec);
        }
    }
    if (!ec) {
        fs::create_directory(scratch / "media", ec);
    }
    if (ec) {
        return std::unexpected(scratch.string() + ": " + ec.message());
    }
    return {};
}

} // namespace live
