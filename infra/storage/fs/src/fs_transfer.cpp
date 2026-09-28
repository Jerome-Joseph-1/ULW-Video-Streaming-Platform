#include "infra/storage/fs_transfer.hpp"

#include "os/unique_fd.hpp"

#include <sys/sendfile.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace infra::storage {

namespace {

namespace fs = std::filesystem;
using core::ports::StorageError;

StorageError from_error_code(const std::error_code& ec) noexcept {
    if (ec == std::errc::no_such_file_or_directory) {
        return StorageError::NotFound;
    }
    if (ec == std::errc::permission_denied) {
        return StorageError::Unauthorized;
    }
    return StorageError::Permanent;
}

} // namespace

FsTransfer::FsTransfer(const fs::path& root) : objects_(root / "objects") {}

fs::path FsTransfer::object_path(const core::StorageKey& key) const {
    return objects_ / key.str();
}

std::expected<std::uint64_t, StorageError> FsTransfer::size(const core::StorageKey& key) {
    std::error_code ec;
    const auto bytes = fs::file_size(object_path(key), ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return bytes;
}

std::expected<std::uint64_t, StorageError> FsTransfer::download(const core::StorageKey& key,
                                                                const fs::path& destination) {
    std::error_code ec;
    fs::copy_file(object_path(key), destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    const auto bytes = fs::file_size(destination, ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    return bytes;
}

std::expected<void, StorageError> FsTransfer::upload(const fs::path& source,
                                                     const core::StorageKey& key,
                                                     const core::ContentType& /*type*/) {
    const fs::path target = object_path(key);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    // A name of its own per writer, ending in ".tmp" so FsStore::list() skips it: two workers
    // that both publish a key after a lease handover must not write into one temporary.
    std::string temp = target.string() + ".XXXXXX.tmp";
    constexpr int kSuffix = 4;
    const os::UniqueFd fd(::mkostemps(temp.data(), kSuffix, O_CLOEXEC));
    if (!fd) {
        return std::unexpected(from_error_code(std::error_code(errno, std::generic_category())));
    }
    const auto discard = [&temp](const std::error_code& why) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return std::unexpected(from_error_code(why));
    };
    // Never through a link: the worker uploads what a sandboxed ffmpeg wrote, and a link there
    // could name any file the worker can read.
    const os::UniqueFd in(::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!in) {
        return discard(std::error_code(errno, std::generic_category()));
    }
    ssize_t copied = 0;
    // sendfile moves under 2 GiB a call; the loop takes the rest.
    constexpr std::size_t kChunk = std::size_t{1} << 30U;
    while ((copied = ::sendfile(fd.get(), in.get(), nullptr, kChunk)) > 0) {
    }
    if (copied < 0) {
        return discard(std::error_code(errno, std::generic_category()));
    }
    // Readers see the old object or the new one: flushed first, then renamed over the target.
    if (::fsync(fd.get()) != 0 || ::rename(temp.c_str(), target.c_str()) != 0) {
        return discard(std::error_code(errno, std::generic_category()));
    }
    return {};
}

} // namespace infra::storage
