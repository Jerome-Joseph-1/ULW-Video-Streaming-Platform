#pragma once

#include "postgres_harness.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ulw::test {

struct Clip {
    std::string size = "1280x720";
    // As ffmpeg reads it: a rational such as 30000/1001.
    std::string rate = "30000/1001";
    int seconds = 4;
    bool audio = true;
};

// Generates a test clip from ffmpeg's lavfi sources, so no media file is committed.
inline bool make_clip(const std::filesystem::path& out, const Clip& clip) {
    std::vector<std::string> argv{
        "ffmpeg", "-nostdin", "-v",
        "error",  "-y",       "-f",
        "lavfi",  "-i",       "testsrc2=size=" + clip.size + ":rate=" + clip.rate};
    if (clip.audio) {
        argv.insert(argv.end(), {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"});
    }
    argv.insert(argv.end(), {"-t", std::to_string(clip.seconds), "-c:v", "libx264", "-preset",
                             "ultrafast", "-pix_fmt", "yuv420p"});
    if (clip.audio) {
        argv.insert(argv.end(), {"-c:a", "aac", "-shortest"});
    }
    argv.push_back(out.string());
    return run_process(argv).exit_code == 0;
}

// Keyframe times of a rendition, read with ffprobe directly rather than through the adapter.
inline std::string keyframe_times(const std::filesystem::path& playlist) {
    return run_process({"ffprobe", "-v", "error", "-skip_frame", "nokey", "-select_streams", "v:0",
                        "-show_entries", "frame=pts_time", "-of", "csv=p=0", playlist.string()})
        .output;
}

} // namespace ulw::test
