#include "infra/storage/fs_transfer.hpp"

#include "os/unique_fd.hpp"

#include <sys/sendfile.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <span>
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

std::unexpected<StorageError> from_errno(int error) noexcept {
    return std::unexpected(from_error_code(std::error_code(error, std::generic_category())));
}

// A new file beside `target`, of a name of its own per writer and ending in ".tmp" so
// FsStore::list() skips it: two workers that both publish a key after a lease handover must not
// write into one temporary.
struct Temporary {
    std::string path;
    os::UniqueFd fd;
};

std::expected<Temporary, StorageError> make_temporary(const fs::path& target) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected(from_error_code(ec));
    }
    std::string temp = target.string() + ".XXXXXX.tmp";
    constexpr int kSuffix = 4;
    os::UniqueFd fd(::mkostemps(temp.data(), kSuffix, O_CLOEXEC));
    if (!fd) {
        return from_errno(errno);
    }
    return Temporary{.path = std::move(temp), .fd = std::move(fd)};
}

// Readers see the old object or the new one: flushed first, then renamed over the target.
// A create-only placement links instead, which the kernel refuses when the target exists, so
// of two racing writers one wins; the temporary is then dropped either way.
std::expected<void, StorageError> install(const Temporary& temp, const fs::path& target,
                                          bool create_only) {
    const auto discard = [&temp](int error) {
        std::error_code ignored;
        fs::remove(temp.path, ignored);
        return from_errno(error);
    };
    if (::fsync(temp.fd.get()) != 0) {
        return discard(errno);
    }
    if (create_only) {
        const int linked = ::link(temp.path.c_str(), target.c_str());
        const int linked_errno = errno;
        std::error_code ignored;
        fs::remove(temp.path, ignored);
        if (linked != 0) {
            return linked_errno == EEXIST ? std::unexpected(StorageError::AlreadyExists)
                                          : from_errno(linked_errno);
        }
    } else if (::rename(temp.path.c_str(), target.c_str()) != 0) {
        return discard(errno);
    }
    // The rename lives in the directory; until that is flushed, a crash can bring back the old
    // object, or none, after we reported the new one, and the worker marks a video ready on
    // the strength of that report.
    const os::UniqueFd dir(
        ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir || ::fsync(dir.get()) != 0) {
        return from_errno(errno);
    }
    return {};
}

class FsObjectStream final : public core::ports::IObjectStream {
public:
    FsObjectStream(Temporary temp, fs::path target) noexcept
        : temp_(std::move(temp)), target_(std::move(target)) {}
    ~FsObjectStream() override {
        if (!committed_) {
            std::error_code ignored;
            fs::remove(temp_.path, ignored);
        }
    }
    FsObjectStream(const FsObjectStream&) = delete;
    FsObjectStream& operator=(const FsObjectStream&) = delete;
    FsObjectStream(FsObjectStream&&) = delete;
    FsObjectStream& operator=(FsObjectStream&&) = delete;

    std::expected<void, StorageError> write(std::span<const std::byte> bytes) override {
        while (!bytes.empty()) {
            const ssize_t n = ::write(temp_.fd.get(), bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0) {
                return from_errno(errno);
            }
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
        return {};
    }

    std::expected<void, StorageError> commit() override {
        if (committed_) {
            return {};
        }
        auto installed = install(temp_, target_, false);
        // install() removed the temporary when it failed; either way it is not ours any more.
        committed_ = true;
        return installed;
    }

private:
    Temporary temp_;
    fs::path target_;
    bool committed_ = false;
};

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
    return place(source, key, false);
}

std::expected<void, StorageError> FsTransfer::upload_new(const fs::path& source,
                                                         const core::StorageKey& key,
                                                         const core::ContentType& /*type*/) {
    return place(source, key, true);
}

std::expected<void, StorageError> FsTransfer::place(const fs::path& source,
                                                    const core::StorageKey& key, bool create_only) {
    const fs::path target = object_path(key);
    auto temp = make_temporary(target);
    if (!temp) {
        return std::unexpected(temp.error());
    }
    const auto discard = [&temp](int error) {
        std::error_code ignored;
        fs::remove(temp->path, ignored);
        return from_errno(error);
    };
    // Never through a link: the worker uploads what a sandboxed ffmpeg wrote, and a link there
    // could name any file the worker can read.
    const os::UniqueFd in(::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!in) {
        return discard(errno);
    }
    ssize_t copied = 0;
    // sendfile moves under 2 GiB a call; the loop takes the rest.
    constexpr std::size_t kChunk = std::size_t{1} << 30U;
    while ((copied = ::sendfile(temp->fd.get(), in.get(), nullptr, kChunk)) > 0) {
    }
    if (copied < 0) {
        return discard(errno);
    }
    return install(*temp, target, create_only);
}

std::expected<std::unique_ptr<core::ports::IObjectStream>, StorageError>
FsTransfer::begin(const core::StorageKey& key, const core::ContentType& /*type*/) {
    fs::path target = object_path(key);
    auto temp = make_temporary(target);
    if (!temp) {
        return std::unexpected(temp.error());
    }
    return std::make_unique<FsObjectStream>(std::move(*temp), std::move(target));
}

} // namespace infra::storage
