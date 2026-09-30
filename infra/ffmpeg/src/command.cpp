#include "command.hpp"

#include "core/util/parse.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace infra::ffmpeg {

namespace {

using core::ports::FrameRate;
using core::ports::MediaInfo;

// Far past kMaxDuration, and small enough that its milliseconds cannot overflow.
constexpr std::uint64_t kMaxParsedSeconds = std::uint64_t{1} << 32U;

std::optional<std::pair<std::string_view, std::string_view>> split_line(std::string_view line) {
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{line.substr(0, eq), line.substr(eq + 1)};
}

template <class Visit> void for_each_line(std::string_view text, Visit visit) {
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        visit(line);
    }
}

std::optional<FrameRate> parse_rate(std::string_view text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto num = core::parse_integer<std::uint32_t>(text.substr(0, slash));
    const auto den = core::parse_integer<std::uint32_t>(text.substr(slash + 1));
    if (!num || !den || *num == 0 || *den == 0) {
        return std::nullopt;
    }
    return FrameRate{.num = *num, .den = *den};
}

// "6.000000" -> 6000 ms. Digits past the millisecond are dropped, not rounded.
std::optional<core::Millis> parse_seconds(std::string_view text) {
    const std::size_t dot = text.find('.');
    const auto whole = core::parse_integer<std::uint64_t>(text.substr(0, dot));
    if (!whole || *whole > kMaxParsedSeconds) {
        return std::nullopt;
    }
    std::uint64_t millis = 0;
    if (dot != std::string_view::npos) {
        const std::string_view fraction = text.substr(dot + 1);
        if (fraction.empty() ||
            !std::ranges::all_of(fraction, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        std::string first_three(fraction.substr(0, 3));
        first_three.resize(3, '0');
        millis = core::parse_integer<std::uint64_t>(first_three).value_or(0);
    }
    return core::Millis{static_cast<std::int64_t>((*whole * 1000) + millis)};
}

std::expected<core::Millis, std::string>
checked_duration(const std::optional<core::Millis>& duration, std::uint64_t source_bytes) {
    if (!duration || *duration <= core::Millis::zero()) {
        return std::unexpected("no duration");
    }
    if (*duration > kMaxDuration) {
        return std::unexpected("longer than the 12 hour maximum");
    }
    constexpr std::uint64_t kBitsPerByte = 8;
    constexpr std::uint64_t kMillisPerSecond = 1000;
    const auto declared = static_cast<std::uint64_t>(duration->count());
    if (declared * kMinSourceBitsPerSecond / kMillisPerSecond / kBitsPerByte > source_bytes) {
        return std::unexpected("declared duration is too long for the file size");
    }
    return *duration;
}

struct Stream {
    std::string_view type;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<FrameRate> rate;
    std::int32_t rotation = 0;
};

std::string kbps(std::uint32_t value) {
    return std::to_string(value) + "k";
}

} // namespace

std::uint32_t gop_frames(FrameRate rate) noexcept {
    // round(4 * num / den) in integers: (2 * 4 * num + den) / (2 * den).
    const std::uint64_t num = (std::uint64_t{2} * kSegmentSeconds * rate.num) + rate.den;
    const std::uint64_t den = std::uint64_t{2} * rate.den;
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(num / den));
}

Args probe_args(const std::string& ffprobe, const std::filesystem::path& input) {
    return {ffprobe,
            "-v",
            "error",
            "-show_entries",
            "stream=codec_type,width,height,r_frame_rate:stream_side_data=rotation:format=duration",
            "-of",
            "default=nw=1",
            "-format_whitelist",
            std::string(kSourceFormats),
            input.string()};
}

std::expected<MediaInfo, std::string> parse_probe(std::string_view text,
                                                  std::uint64_t source_bytes) {
    std::optional<Stream> video;
    bool has_audio = false;
    std::optional<Stream> current;
    std::optional<core::Millis> duration;
    const auto close_stream = [&] {
        if (current && current->type == "video" && !video) {
            video = current;
        }
        if (current && current->type == "audio") {
            has_audio = true;
        }
        current.reset();
    };
    for_each_line(text, [&](std::string_view line) {
        const auto kv = split_line(line);
        if (!kv) {
            return;
        }
        const auto [key, value] = *kv;
        if (key == "codec_type") {
            close_stream();
            current = Stream{.type = value, .width = {}, .height = {}, .rate = {}, .rotation = 0};
        } else if (key == "duration") {
            duration = parse_seconds(value);
        } else if (!current) {
            return;
        } else if (key == "width") {
            current->width = core::parse_integer<std::uint32_t>(value);
        } else if (key == "height") {
            current->height = core::parse_integer<std::uint32_t>(value);
        } else if (key == "r_frame_rate") {
            current->rate = parse_rate(value);
        } else if (key == "rotation") {
            current->rotation = core::parse_integer<std::int32_t>(value).value_or(0);
        }
    });
    close_stream();
    if (!video) {
        return std::unexpected("no video stream");
    }
    if (!video->width || !video->height || *video->width == 0 || *video->height == 0) {
        return std::unexpected("video stream has no dimensions");
    }
    if (!video->rate) {
        return std::unexpected("video stream has no frame rate");
    }
    const auto length = checked_duration(duration, source_bytes);
    if (!length) {
        return std::unexpected(length.error());
    }
    // ffmpeg applies the container's display rotation while decoding, so a quarter turn
    // swaps the dimensions the ladder has to fit.
    const bool quarter_turn = video->rotation % 180 != 0;
    return MediaInfo{.width = quarter_turn ? *video->height : *video->width,
                     .height = quarter_turn ? *video->width : *video->height,
                     .frame_rate = *video->rate,
                     .duration = *length,
                     .has_audio = has_audio};
}

Args transcode_args(const std::string& ffmpeg, const std::filesystem::path& input,
                    const std::filesystem::path& out_dir, const MediaInfo& media,
                    std::span<const core::Rung> ladder, unsigned threads) {
    Args args{ffmpeg,    "-nostdin",    "-hide_banner",      "-loglevel",
              "warning", "-y",          "-format_whitelist", std::string(kSourceFormats),
              "-i",      input.string()};

    std::string graph = "[0:v]split=" + std::to_string(ladder.size());
    for (std::size_t i = 1; i <= ladder.size(); ++i) {
        graph += "[v" + std::to_string(i) + "]";
    }
    for (std::size_t i = 1; i <= ladder.size(); ++i) {
        const std::string n = std::to_string(i);
        graph += std::format(";[v{}]scale=-2:{}[v{}o]", n, ladder[i - 1].height, n);
    }
    args.insert(args.end(), {"-filter_complex", graph});

    for (std::size_t i = 0; i < ladder.size(); ++i) {
        const std::string v = ":v:" + std::to_string(i);
        const std::uint32_t target = ladder[i].video_kbps;
        // The spec's table in ratios: peaks 7% over the target, and a VBV buffer of 1.5 s
        // at the target rate.
        const std::uint32_t maxrate = target * 107 / 100;
        const std::uint32_t bufsize = target * 3 / 2;
        args.insert(args.end(),
                    {"-map", "[v" + std::to_string(i + 1) + "o]", "-c" + v, "libx264", "-b" + v,
                     kbps(target), "-maxrate" + v, kbps(maxrate), "-bufsize" + v, kbps(bufsize)});
    }
    if (media.has_audio) {
        for (std::size_t i = 0; i < ladder.size(); ++i) {
            args.insert(args.end(), {"-map", "a:0"});
        }
        args.insert(args.end(),
                    {"-c:a", "aac", "-b:a", kbps(core::kAudioKbps), "-ac", "2", "-ar", "48000"});
    }

    const std::string gop = std::to_string(gop_frames(media.frame_rate));
    const std::string segment = std::to_string(kSegmentSeconds);
    // No B-frames. With them each keyframe is decoded two frames before it is shown, and the
    // HLS muxer, writing packets in decode order, closes a segment's audio at the keyframe's
    // decode time: every segment's audio then ended 66 ms (at 30 fps) short of its video. A
    // player's combined audio+video buffer only covers where both do, so no segment was ever
    // fully buffered, and a rung switch that re-appends one over buffered media reads as an
    // append that made no progress (hls.js's bufferAppendNoProgress).
    args.insert(args.end(),
                {"-preset", "veryfast", "-profile:v", "main", "-level", "4.0", "-pix_fmt",
                 "yuv420p", "-bf", "0", "-sc_threshold", "0", "-g", gop, "-keyint_min", gop,
                 "-force_key_frames", "expr:gte(t,n_forced*" + segment + ")"});

    std::string streams;
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        const std::string n = std::to_string(i);
        streams += (i == 0 ? "" : " ") + ("v:" + n) + (media.has_audio ? ",a:" + n : "") +
                   ",name:" + ladder[i].name;
    }
    args.insert(args.end(), {"-hls_time",
                             segment,
                             "-hls_playlist_type",
                             "vod",
                             "-hls_segment_type",
                             "fmp4",
                             "-hls_flags",
                             "independent_segments",
                             "-master_pl_name",
                             "master.m3u8",
                             "-var_stream_map",
                             streams,
                             "-hls_segment_filename",
                             (out_dir / "%v" / "seg_%05d.m4s").string(),
                             "-threads",
                             std::to_string(threads),
                             "-progress",
                             "pipe:1",
                             "-nostats",
                             (out_dir / "%v" / "index.m3u8").string()});
    return args;
}

Args keyframe_args(const std::string& ffprobe, const std::filesystem::path& playlist) {
    return {ffprobe,
            "-v",
            "error",
            "-skip_frame",
            "nokey",
            "-select_streams",
            "v:0",
            "-show_entries",
            "frame=pts_time",
            "-of",
            "default=nw=1:nk=1",
            "-format_whitelist",
            std::string(kOutputFormats),
            playlist.string()};
}

std::optional<std::vector<std::string>> parse_keyframes(std::string_view text) {
    std::vector<std::string> times;
    bool valid = true;
    for_each_line(text, [&](std::string_view line) {
        if (line.empty()) {
            return;
        }
        const bool time_like = std::ranges::all_of(
            line, [](char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-'; });
        if (!time_like) {
            valid = false;
            return;
        }
        times.emplace_back(line);
    });
    if (!valid) {
        return std::nullopt;
    }
    return times;
}

Args decode_args(const std::string& ffmpeg, const std::filesystem::path& master) {
    return {ffmpeg,
            "-nostdin",
            "-v",
            "error",
            "-format_whitelist",
            std::string(kOutputFormats),
            "-i",
            master.string(),
            "-f",
            "null",
            "-"};
}

std::optional<std::string> check_media_playlist(std::string_view text) {
    if (!text.starts_with("#EXTM3U")) {
        return "not a playlist";
    }
    // fMP4 segments need version 7 and an initialisation segment; a VOD playlist must end.
    for (const std::string_view tag : {"#EXT-X-VERSION:7", "#EXT-X-MAP:URI=", "#EXT-X-ENDLIST"}) {
        if (text.find(tag) == std::string_view::npos) {
            return "media playlist lacks " + std::string(tag);
        }
    }
    if (text.find("#EXTINF:") == std::string_view::npos) {
        return "media playlist has no segments";
    }
    return std::nullopt;
}

namespace {

// An attribute list with BANDWIDTH replaced and AVERAGE-BANDWIDTH dropped. Commas inside a
// quoted value (CODECS="avc1...,mp4a...") do not separate attributes.
std::string with_bandwidth(std::string_view attributes, std::uint64_t bandwidth) {
    std::string out;
    const auto keep = [&](std::string_view attribute) {
        const std::string_view name = attribute.substr(0, attribute.find('='));
        if (name == "AVERAGE-BANDWIDTH") {
            return;
        }
        if (!out.empty()) {
            out += ',';
        }
        if (name == "BANDWIDTH") {
            out += "BANDWIDTH=" + std::to_string(bandwidth);
        } else {
            out += attribute;
        }
    };
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        if (attributes[i] == '"') {
            quoted = !quoted;
        } else if (attributes[i] == ',' && !quoted) {
            keep(attributes.substr(start, i - start));
            start = i + 1;
        }
    }
    keep(attributes.substr(start));
    return out;
}

} // namespace

std::string settle_master_bandwidth(std::string_view text, std::span<const core::Rung> ladder,
                                    bool has_audio) {
    constexpr std::string_view kStreamInf = "#EXT-X-STREAM-INF:";
    std::string out;
    out.reserve(text.size());
    // A variant's tag is followed by its URI, which says which rung it is.
    std::optional<std::string_view> tag;
    const auto flush = [&] {
        if (tag) {
            out.append(*tag).push_back('\n');
            tag.reset();
        }
    };
    for_each_line(text, [&](std::string_view line) {
        if (tag && !line.empty() && !line.starts_with('#')) {
            const auto rung = std::ranges::find_if(
                ladder, [&](const core::Rung& r) { return line == r.name + "/index.m3u8"; });
            if (rung != ladder.end()) {
                const std::uint64_t kbps =
                    std::uint64_t{rung->video_kbps} + (has_audio ? core::kAudioKbps : 0U);
                out.append(kStreamInf)
                    .append(with_bandwidth(tag->substr(kStreamInf.size()), kbps * 1000 * 11 / 10))
                    .push_back('\n');
                tag.reset();
            }
        }
        flush();
        if (line.starts_with(kStreamInf)) {
            tag = line;
        } else {
            out.append(line).push_back('\n');
        }
    });
    flush();
    return out;
}

std::optional<std::string>
check_master_playlist(std::string_view text, std::span<const core::Rung> ladder, bool has_audio) {
    if (!text.starts_with("#EXTM3U") || text.find("#EXT-X-VERSION:7") == std::string_view::npos) {
        return "master playlist lacks #EXT-X-VERSION:7";
    }
    std::size_t variants = 0;
    bool codecs = true;
    for_each_line(text, [&](std::string_view line) {
        if (line.starts_with("#EXT-X-STREAM-INF:")) {
            ++variants;
            codecs = codecs && line.find("CODECS=\"avc1.") != std::string_view::npos;
        }
    });
    if (variants != ladder.size()) {
        return "master playlist has " + std::to_string(variants) + " variants, expected " +
               std::to_string(ladder.size());
    }
    if (!codecs) {
        return "master playlist variant without CODECS";
    }
    for (const core::Rung& rung : ladder) {
        if (text.find("\n" + rung.name + "/index.m3u8") == std::string_view::npos) {
            return "master playlist does not reference " + rung.name;
        }
    }
    if (settle_master_bandwidth(text, ladder, has_audio) != text) {
        return "master playlist BANDWIDTH is not the ladder's";
    }
    return std::nullopt;
}

} // namespace infra::ffmpeg
