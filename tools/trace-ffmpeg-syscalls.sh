#!/usr/bin/env bash
# Prints the distinct system calls ffprobe and ffmpeg make on the worker's, the live packager's
# and the live recording remux's own command lines, for comparing with the allowlist in
# infra/ffmpeg/src/seccomp_filter.hpp (docs/adr/0048). The worker's probe and transcode run over
# one source per decoder its formats admit in practice: H.264/AAC in MP4, HEVC/AAC in MP4,
# VP9/Opus in WebM, AV1/AAC in Matroska and MPEG-2/MP2 in MPEG-PS. Run it on every ffmpeg or
# base image bump, once as root and once as an ordinary user: they differ (a library asks about
# setuid only when it is not root). Needs strace, ffmpeg with those encoders, and setpriv
# (util-linux's, the `setpriv` package on Debian) when given a uid.
#
#   tools/trace-ffmpeg-syscalls.sh [uid]    # no uid: as the current user
set -euo pipefail

uid=${1:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
chmod 777 "$work"
run=()
[[ -n $uid ]] && run=(setpriv --reuid="$uid" --regid="$uid" --clear-groups)

ffmpeg -nostdin -v error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi \
    -i sine=frequency=440 -t 3 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac \
    -shortest "$work/in.mp4"
chmod 644 "$work/in.mp4"
ffmpeg -nostdin -v error -y -i "$work/in.mp4" -c:v copy -c:a copy -f mpegts "$work/in.ts"
formats=mov,matroska,mpegts,avi,flv,asf,mpeg,ogg
mkdir -p "$work"/out/live
chmod -R 777 "$work/out"

# The worker's probe and three-rung transcode (command.cpp) of one source; TAG names the traces.
trace_source() {
    local tag=$1 input=$2
    mkdir -p "$work/out-$tag"/{1080p,720p,360p}
    chmod -R 777 "$work/out-$tag"
    "${run[@]}" strace -f -qq -o "$work/probe-$tag.txt" ffprobe -v error \
        -show_entries \
        stream=codec_type,width,height,r_frame_rate:stream_side_data=rotation:format=duration \
        -of default=nw=1 -format_whitelist "$formats" "$input" >/dev/null
    "${run[@]}" strace -f -qq -o "$work/transcode-$tag.txt" ffmpeg -nostdin -hide_banner \
        -loglevel warning -y -format_whitelist "$formats" -i "$input" \
        -filter_complex "[0:v]split=3[v1][v2][v3];[v1]scale=-2:1080[v1o];[v2]scale=-2:720[v2o];[v3]scale=-2:360[v3o]" \
        -map "[v1o]" -c:v:0 libx264 -b:v:0 5000k -maxrate:v:0 5350k -bufsize:v:0 7500k \
        -map "[v2o]" -c:v:1 libx264 -b:v:1 2800k -maxrate:v:1 2996k -bufsize:v:1 4200k \
        -map "[v3o]" -c:v:2 libx264 -b:v:2 800k -maxrate:v:2 856k -bufsize:v:2 1200k \
        -map a:0 -map a:0 -map a:0 -c:a aac -b:a 128k -ac 2 -ar 48000 \
        -preset veryfast -profile:v main -level 4.0 -pix_fmt yuv420p -bf 0 -sc_threshold 0 \
        -g 120 -keyint_min 120 -force_key_frames 'expr:gte(t,n_forced*4)' \
        -hls_time 4 -hls_playlist_type vod -hls_segment_type fmp4 \
        -hls_flags independent_segments -master_pl_name master.m3u8 \
        -var_stream_map "v:0,a:0,name:1080p v:1,a:1,name:720p v:2,a:2,name:360p" \
        -hls_segment_filename "$work/out-$tag/%v/seg_%05d.m4s" -threads 4 -progress pipe:1 \
        -nostats "$work/out-$tag/%v/index.m3u8" >/dev/null
}

# One second each of the other decoders' inputs, small: the decoder's start-up and threads
# are what is traced, not its throughput.
source=(-f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i sine=frequency=440 -t 1
    -pix_fmt yuv420p)
av1=libsvtav1
ffmpeg -hide_banner -encoders 2>/dev/null | grep -qw libsvtav1 || av1=libaom-av1
ffmpeg -nostdin -v error -y "${source[@]}" -c:v libx265 -preset ultrafast -tag:v hvc1 \
    -c:a aac "$work/in-hevc.mp4"
ffmpeg -nostdin -v error -y "${source[@]}" -c:v libvpx-vp9 -deadline realtime -cpu-used 8 \
    -c:a libopus "$work/in-vp9.webm"
ffmpeg -nostdin -v error -y "${source[@]}" -c:v "$av1" -preset 12 -cpu-used 8 -c:a aac \
    "$work/in-av1.mkv"
ffmpeg -nostdin -v error -y "${source[@]}" -c:v mpeg2video -c:a mp2 -f mpeg "$work/in-mpeg2.mpg"
chmod 644 "$work"/in-*
trace_source h264 "$work/in.mp4"
trace_source hevc "$work/in-hevc.mp4"
trace_source vp9 "$work/in-vp9.webm"
trace_source av1 "$work/in-av1.mkv"
trace_source mpeg2 "$work/in-mpeg2.mpg"

# The worker's checks of its own output (command.cpp): keyframe times and a full decode.
"${run[@]}" strace -f -qq -o "$work/keyframes.txt" ffprobe -v error -skip_frame nokey \
    -select_streams v:0 -show_entries frame=pts_time -of default=nw=1:nk=1 \
    -format_whitelist hls,mov "$work/out-h264/720p/index.m3u8" >/dev/null
"${run[@]}" strace -f -qq -o "$work/decode.txt" ffmpeg -nostdin -v error \
    -format_whitelist hls,mov -i "$work/out-h264/master.m3u8" -f null -
# The live packager's remux (infra/ffmpeg/src/live_command.cpp), its input on a pipe as there.
"${run[@]}" strace -f -qq -o "$work/live.txt" ffmpeg -nostdin -hide_banner -loglevel warning \
    -nostats -analyzeduration 1000000 -probesize 1000000 -f mpegts -i pipe:0 -map 0:v:0 \
    -map '0:a:0?' -c copy -bsf:a aac_adtstoasc -f hls -hls_time 1 -hls_list_size 2 \
    -hls_segment_type fmp4 -hls_fmp4_init_filename init_0.mp4 \
    -hls_segment_filename "$work/out/live/seg_0_%d.m4s" -start_number 0 \
    -hls_flags independent_segments+temp_file "$work/out/live/index.m3u8" <"$work/in.ts"

# The live recording's remux (recording_remux.cpp): the audio probe of an init segment, then a
# fragmented MP4 to MPEG-TS on pipes, once as it is and once with silence added, and MPEG-TS to
# MPEG-TS.
fragmented=frag_keyframe+empty_moov+default_base_moof
ffmpeg -nostdin -v error -y -i "$work/in.mp4" -c copy -movflags "$fragmented" -f mp4 \
    "$work/in.frag.mp4"
ffmpeg -nostdin -v error -y -i "$work/in.mp4" -an -c copy -movflags "$fragmented" -f mp4 \
    "$work/in.video.frag.mp4"
chmod 644 "$work"/in.*
"${run[@]}" strace -f -qq -o "$work/recording-probe.txt" ffprobe -v error -select_streams a:0 \
    -show_entries stream=sample_rate,channels -of default=noprint_wrappers=1 "$work/in.frag.mp4" \
    >/dev/null
"${run[@]}" strace -f -qq -o "$work/recording-mp4.txt" ffmpeg -nostdin -hide_banner \
    -loglevel error -nostats -f mp4 -i pipe:0 -map 0:v:0 -map '0:a:0?' -c copy -f mpegts pipe:1 \
    <"$work/in.frag.mp4" >/dev/null
"${run[@]}" strace -f -qq -o "$work/recording-silence.txt" ffmpeg -nostdin -hide_banner \
    -loglevel error -nostats -f mp4 -i pipe:0 -f lavfi -i anullsrc=r=48000:cl=stereo -map 0:v:0 \
    -map 1:a:0 -shortest -c:v copy -c:a aac -f mpegts pipe:1 <"$work/in.video.frag.mp4" >/dev/null
"${run[@]}" strace -f -qq -o "$work/recording-ts.txt" ffmpeg -nostdin -hide_banner -loglevel error \
    -nostats -f mpegts -i pipe:0 -map 0:v:0 -map '0:a:0?' -c copy -f mpegts pipe:1 \
    <"$work/in.ts" >/dev/null

cat "$work"/probe-*.txt "$work"/transcode-*.txt "$work"/{keyframes,decode,live}.txt \
    "$work"/recording-{probe,mp4,silence,ts}.txt |
    sed -E 's/^[0-9]+ +//; s/^<\.\.\. ([a-z_0-9]+) resumed>.*/\1(/' |
    grep -oE '^[a-z_0-9]+\(' | tr -d '(' | sort -u
