#pragma once

#include "core/ports/object_transfer.hpp"
#include "infra/storage/fs_transfer.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ulw::test {

// The filesystem store, plus a record of every upload in order and faults to inject.
class RecordingStore final : public core::ports::IObjectTransfer {
public:
    explicit RecordingStore(const std::filesystem::path& root) : inner_(root) {}

    std::expected<std::uint64_t, core::ports::StorageError>
    size(const core::StorageKey& key) override {
        if (size_error) {
            return std::unexpected(*size_error);
        }
        return inner_.size(key);
    }
    std::expected<std::uint64_t, core::ports::StorageError>
    download(const core::StorageKey& key, const std::filesystem::path& destination) override {
        if (download_error) {
            return std::unexpected(*download_error);
        }
        return inner_.download(key, destination);
    }
    std::expected<void, core::ports::StorageError> upload(const std::filesystem::path& source,
                                                          const core::StorageKey& key,
                                                          const core::ContentType& type) override {
        const std::string name = name_of(key);
        if (fail_all_uploads || fail_once.erase(name) != 0) {
            return std::unexpected(core::ports::StorageError::Transient);
        }
        uploads.push_back(name);
        content_types[name] = std::string(type.view());
        return inner_.upload(source, key, type);
    }
    std::expected<void, core::ports::StorageError>
    upload_new(const std::filesystem::path& source, const core::StorageKey& key,
               const core::ContentType& type) override {
        auto placed = inner_.upload_new(source, key, type);
        if (placed) {
            claims.push_back(name_of(key));
            if (after_claim) {
                after_claim(name_of(key));
            }
        }
        return placed;
    }

    static std::string name_of(const core::StorageKey& key) {
        return key.str().substr(key.str().rfind('/') + 1);
    }

    // Runs after each claim succeeds, with its name: another packager acting in between.
    std::function<void(const std::string&)> after_claim;
    std::vector<std::string> uploads;
    std::vector<std::string> claims;
    std::map<std::string, std::string> content_types;
    std::set<std::string> fail_once;
    bool fail_all_uploads = false;
    std::optional<core::ports::StorageError> download_error;
    std::optional<core::ports::StorageError> size_error;

private:
    infra::storage::FsTransfer inner_;
};

// What ffmpeg writes for segments first..first+count-1 of an epoch, all closed.
inline std::string ffmpeg_playlist(std::uint32_t epoch, std::uint64_t first, std::uint64_t count,
                                   const std::string& init, double seconds = 2.0) {
    std::ostringstream text;
    text << "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:" << first
         << "\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"" << init << "\"\n";
    for (std::uint64_t n = first; n < first + count; ++n) {
        text.precision(6);
        text << "#EXTINF:" << std::fixed << seconds << ",\nseg_" << epoch << "_" << n << ".m4s\n";
    }
    return text.str();
}

inline void write_file(const std::filesystem::path& file, std::string_view bytes = "bytes") {
    std::ofstream(file, std::ios::binary) << bytes;
}

} // namespace ulw::test
