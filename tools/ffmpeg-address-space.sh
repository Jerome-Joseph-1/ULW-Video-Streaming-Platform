#!/usr/bin/env bash
# Prints the peak address space (VmPeak) of each ffprobe and ffmpeg command line the sandbox
# runs, next to the RLIMIT_AS it runs under (infra/ffmpeg/src: transcoder.cpp, live_remux.cpp,
# recording_remux.cpp) and with the environment it gets there (MALLOC_ARENA_MAX for the
# remuxes, process.hpp), and fails if one exits non-zero under its limit. Run it on every
# ffmpeg or base image bump, beside tools/trace-ffmpeg-syscalls.sh (docs/adr/0074): a newer
# ffmpeg or glibc maps more, and a child that reaches its limit fails its job for good.
# The peak depends on the core count (threads, and an arena per thread), so say where it ran.
#
#   tools/ffmpeg-address-space.sh [--full]   # --full adds the 60 s 1080p transcode (minutes)
set -euo pipefail

full=false
[[ ${1:-} == --full ]] && full=true
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
failures=0
echo "cores: $(nproc); $(ffmpeg -version | head -1)"

# peak LABEL LIMIT_MIB ENV COMMAND: runs COMMAND (one shell string) under RLIMIT_AS=LIMIT with
# ENV ("-" for none), sampling /proc/PID/status until it exits. VmPeak only grows, so the last
# sample is the peak up to a few milliseconds before the end.
peak() {
    local label=$1 limit=$2 env=$3 command=$4 pid peak_kib=0 value status
    local -a environment=()
    [[ $env != - ]] && environment=("$env")
    env "${environment[@]}" prlimit --as=$((limit << 20)) bash -c "exec $command" &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        value=$(awk '/^VmPeak/ {print $2}' "/proc/$pid/status" 2>/dev/null || true)
        [[ -n $value ]] && peak_kib=$value
        sleep 0.02
    done
    status=0
    wait "$pid" || status=$?
    printf '%-44s %5d MiB of %5d MiB  exit %d\n' "$label" $((peak_kib >> 10)) "$limit" "$status"
    [[ $status -eq 0 ]] || failures=$((failures + 1))
}

ff="ffmpeg -nostdin -v error -y"
$ff -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 -t 10 \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 60 -c:a aac in.mp4
$ff -i in.mp4 -c copy -f mpegts in.ts
$ff -i in.mp4 -an -c copy -movflags frag_keyframe+empty_moov+default_base_moof -f mp4 \
    video.frag.mp4
$ff -i in.mp4 -c copy -movflags frag_keyframe+empty_moov+default_base_moof -f mp4 frag.mp4
arenas=MALLOC_ARENA_MAX=2

peak "probe" 1024 - "ffprobe -v error -show_entries \
stream=codec_type,width,height,r_frame_rate:stream_side_data=rotation:format=duration \
-of default=nw=1 -format_whitelist mov,matroska,mpegts in.mp4 >/dev/null"
mkdir -p live
peak "live remux" 1024 "$arenas" "ffmpeg -nostdin -hide_banner -loglevel warning -nostats \
-analyzeduration 3000000 -probesize 15000000 -f mpegts -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy \
-bsf:a aac_adtstoasc -f hls -hls_time 2 -hls_list_size 2 -hls_segment_type fmp4 \
-hls_fmp4_init_filename init_0.mp4 -hls_segment_filename live/seg_0_%d.m4s -start_number 0 \
-hls_flags independent_segments+temp_file live/index.m3u8 <in.ts"
peak "recording audio probe" 1024 "$arenas" "ffprobe -v error -select_streams a:0 \
-show_entries stream=sample_rate,channels -of default=noprint_wrappers=1 frag.mp4 >/dev/null"
peak "recording remux, fMP4 to TS" 1024 "$arenas" "ffmpeg -nostdin -hide_banner -loglevel error \
-nostats -f mp4 -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy -f mpegts pipe:1 <frag.mp4 >/dev/null"
peak "recording remux, with silence" 1024 "$arenas" "ffmpeg -nostdin -hide_banner \
-loglevel error -nostats -f mp4 -i pipe:0 -f lavfi -i anullsrc=r=48000:cl=stereo -map 0:v:0 \
-map 1:a:0 -shortest -c:v copy -c:a aac -f mpegts pipe:1 <video.frag.mp4 >/dev/null"
peak "recording remux, TS to TS" 1024 "$arenas" "ffmpeg -nostdin -hide_banner -loglevel error \
-nostats -f mpegts -i pipe:0 -map 0:v:0 -map 0:a:0? -c copy -f mpegts pipe:1 <in.ts >/dev/null"

if $full; then
    $ff -f lavfi -i testsrc2=size=1920x1080:rate=30 -f lavfi -i sine=frequency=440 -t 60 \
        -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 60 -c:a aac big.mp4
    mkdir -p out/{1080p,720p,360p}
    peak "1080p 60 s transcode, -threads 4" 4096 - "ffmpeg -nostdin -hide_banner \
-loglevel warning -y -i big.mp4 -filter_complex \
'[0:v]split=3[v1][v2][v3];[v1]scale=-2:1080[v1o];[v2]scale=-2:720[v2o];[v3]scale=-2:360[v3o]' \
-map '[v1o]' -c:v:0 libx264 -b:v:0 5000k -maxrate:v:0 5350k -bufsize:v:0 7500k \
-map '[v2o]' -c:v:1 libx264 -b:v:1 2800k -maxrate:v:1 2996k -bufsize:v:1 4200k \
-map '[v3o]' -c:v:2 libx264 -b:v:2 800k -maxrate:v:2 856k -bufsize:v:2 1200k \
-map a:0 -map a:0 -map a:0 -c:a aac -b:a 128k -ac 2 -ar 48000 -preset veryfast -profile:v main \
-level 4.0 -pix_fmt yuv420p -bf 0 -sc_threshold 0 -g 120 -keyint_min 120 -hls_time 4 \
-hls_playlist_type vod -hls_segment_type fmp4 -hls_flags independent_segments \
-master_pl_name master.m3u8 -var_stream_map 'v:0,a:0,name:1080p v:1,a:1,name:720p v:2,a:2,name:360p' \
-hls_segment_filename 'out/%v/seg_%05d.m4s' -threads 4 -nostats 'out/%v/index.m3u8'"
    peak "decode check of that output" 4096 - "ffmpeg -nostdin -v error \
-format_whitelist hls,mov -i out/master.m3u8 -f null -"
fi

if [[ $failures -ne 0 ]]; then
    echo "$failures command line(s) failed under their address space limit" >&2
    exit 1
fi
