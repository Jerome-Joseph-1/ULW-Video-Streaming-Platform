#!/usr/bin/env bash
# Prints the distinct system calls ffprobe and ffmpeg make on the worker's and the live packager's
# own command lines, for comparing with the allowlist in infra/ffmpeg/src/seccomp_filter.hpp
# (docs/adr/0048). Run it on every ffmpeg or base image bump, once as root and once as an ordinary
# user: they differ (a library asks about setuid only when it is not root). Needs strace, ffmpeg
# and setpriv.
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
mkdir -p "$work"/out/{1080p,720p,360p,live}
chmod -R 777 "$work/out"

"${run[@]}" strace -f -qq -o "$work/probe.txt" ffprobe -v error \
    -show_entries stream=codec_type,width,height,r_frame_rate:format=duration -of default=nw=1 \
    -format_whitelist "$formats" "$work/in.mp4" >/dev/null
"${run[@]}" strace -f -qq -o "$work/transcode.txt" ffmpeg -nostdin -hide_banner -loglevel warning \
    -y -format_whitelist "$formats" -i "$work/in.mp4" \
    -filter_complex "[0:v]split=3[v1][v2][v3];[v1]scale=-2:1080[v1o];[v2]scale=-2:720[v2o];[v3]scale=-2:360[v3o]" \
    -map "[v1o]" -c:v:0 libx264 -b:v:0 5000k -map "[v2o]" -c:v:1 libx264 -b:v:1 2800k \
    -map "[v3o]" -c:v:2 libx264 -b:v:2 800k -map a:0 -map a:0 -map a:0 -c:a aac \
    -preset veryfast -bf 0 -g 120 -hls_time 4 -hls_playlist_type vod -hls_segment_type fmp4 \
    -master_pl_name master.m3u8 \
    -var_stream_map "v:0,a:0,name:1080p v:1,a:1,name:720p v:2,a:2,name:360p" \
    -hls_segment_filename "$work/out/%v/seg_%05d.m4s" -threads 4 -progress pipe:1 \
    "$work/out/%v/index.m3u8" >/dev/null
# The live packager's remux (infra/ffmpeg/src/live_command.cpp), its input on a pipe as there.
"${run[@]}" strace -f -qq -o "$work/live.txt" ffmpeg -nostdin -hide_banner -loglevel warning \
    -nostats -analyzeduration 1000000 -probesize 1000000 -f mpegts -i pipe:0 -map 0:v:0 \
    -map '0:a:0?' -c copy -bsf:a aac_adtstoasc -f hls -hls_time 1 -hls_list_size 2 \
    -hls_segment_type fmp4 -hls_fmp4_init_filename init_0.mp4 \
    -hls_segment_filename "$work/out/live/seg_0_%d.m4s" -start_number 0 \
    -hls_flags independent_segments+temp_file "$work/out/live/index.m3u8" <"$work/in.ts"

cat "$work/probe.txt" "$work/transcode.txt" "$work/live.txt" | sed -E 's/^[0-9]+ +//; s/^<\.\.\. ([a-z_0-9]+) resumed>.*/\1(/' |
    grep -oE '^[a-z_0-9]+\(' | tr -d '(' | sort -u
