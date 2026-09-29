#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace ulw::test {

// A fresh directory under the system temp dir, removed with everything in it on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& what = "ulw") {
        std::string tmpl = (std::filesystem::temp_directory_path() / (what + "-XXXXXX")).string();
        const char* made = ::mkdtemp(tmpl.data());
        path_ = made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
    }
    ~TempDir() {
        std::error_code ec;
        if (!path_.empty()) {
            std::filesystem::remove_all(path_, ec);
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace ulw::test
