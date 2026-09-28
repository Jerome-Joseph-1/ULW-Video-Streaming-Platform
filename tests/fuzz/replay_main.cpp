// Feeds every file named on the command line, or found directly inside a named directory, to
// the fuzz entry point once. Builds without libFuzzer use it to keep the committed corpus
// passing.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::filesystem::path> inputs(std::span<char* const> args) {
    std::vector<std::filesystem::path> paths;
    for (const char* arg : args) {
        const std::filesystem::path path{arg};
        if (!std::filesystem::is_directory(path)) {
            paths.push_back(path);
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator{path}) {
            if (entry.is_regular_file()) {
                paths.push_back(entry.path());
            }
        }
    }
    // Directory order is unspecified; a failure should name the same input on every run.
    std::ranges::sort(paths);
    return paths;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::filesystem::path> paths =
        inputs(std::span{argv, static_cast<std::size_t>(argc)}.subspan(1));
    // An empty run would pass without checking anything, as it would after a corpus move.
    if (paths.empty()) {
        std::cerr << "no inputs\n";
        return 1;
    }
    for (const std::filesystem::path& path : paths) {
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            std::cerr << "cannot read " << path << '\n';
            return 1;
        }
        const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{in},
                                              std::istreambuf_iterator<char>{});
        // Printed before the run, so a trap is preceded by the name of the input that caused it.
        std::cout << path.filename().string() << '\n' << std::flush;
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
    std::cout << "replayed " << paths.size() << " inputs\n";
    return 0;
}
