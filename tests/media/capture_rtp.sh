#!/usr/bin/env bash
# The RTP fixture of tests/unit/rtp/pcap_test.cpp: eight seconds of ffmpeg's RTP muxer (VP8 and
# Opus, each stream with its RTCP on the same port, as RFC 5761 multiplexes them) captured on
# loopback, and what tshark reads from each packet.
#
#   tests/media/capture_rtp.sh           capture again and rewrite the pcap and both tables
#   tests/media/capture_rtp.sh --check   rerun tshark on the committed pcap and diff the tables
#
# Capturing needs ffmpeg (libvpx, libopus) and tcpdump with the right to capture on lo; both
# modes need tshark. Plain RTP, not SRTP, so that tshark can read what the parser reads.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
data=$(cd "$here/.." && pwd)/data/rtp
name=ffmpeg_vp8_opus
video_port=41000
audio_port=41002

# tshark finds RTP and RTCP on arbitrary ports only with its heuristics on. Frame numbers tie
# each row to a packet of the pcap.
tables() {
    local pcap=$1 out=$2
    local common=(-r "$pcap" -o rtp.heuristic_rtp:TRUE -o rtcp.heuristic_rtcp:TRUE
        -T fields -E header=y -E separator=/t)
    tshark "${common[@]}" -Y rtp -e frame.number -e rtp.seq -e rtp.timestamp -e rtp.ssrc \
        -e rtp.p_type -e rtp.marker 2>/dev/null >"$out.rtp.tsv"
    tshark "${common[@]}" -Y rtcp -e frame.number -e rtcp.pt -e rtcp.senderssrc \
        -e rtcp.timestamp.rtp -e rtcp.sender.packetcount -e rtcp.sender.octetcount \
        2>/dev/null >"$out.rtcp.tsv"
}

scratch=$(mktemp -d)
capture_pid=
cleanup() {
    [[ -n $capture_pid ]] && kill "$capture_pid" 2>/dev/null || true
    rm -rf "$scratch"
}
trap cleanup EXIT

if [[ ${1:-} == --check ]]; then
    tables "$data/$name.pcap" "$scratch/$name"
    diff -u "$data/$name.rtp.tsv" "$scratch/$name.rtp.tsv"
    diff -u "$data/$name.rtcp.tsv" "$scratch/$name.rtcp.tsv"
    echo "capture_rtp.sh: tables match tshark"
    exit 0
fi

mkdir -p "$data"
tcpdump -i lo -U -w "$scratch/$name.pcap" \
    "udp and (dst port $video_port or dst port $audio_port)" 2>"$scratch/tcpdump.log" &
capture_pid=$!
for _ in $(seq 100); do
    grep -q listening "$scratch/tcpdump.log" && break
    sleep 0.1
done
grep -q listening "$scratch/tcpdump.log" || { cat "$scratch/tcpdump.log" >&2; exit 1; }

# -re and -t on each input: an input without -re is read as fast as ffmpeg can encode it.
# 160x120 at 64 kbit/s keeps the pcap near 100 KB and still splits frames across packets.
ffmpeg -nostdin -v error \
    -re -t 8 -f lavfi -i testsrc2=size=160x120:rate=15 \
    -re -t 8 -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -map 0:v -c:v libvpx -b:v 64k -deadline realtime \
    -f rtp "rtp://127.0.0.1:$video_port?rtcpport=$video_port&pkt_size=1000" \
    -map 1:a -c:a libopus -b:a 24k \
    -f rtp "rtp://127.0.0.1:$audio_port?rtcpport=$audio_port" >/dev/null

# tcpdump -U writes each packet as it arrives; SIGTERM flushes and closes the file.
kill "$capture_pid"
wait "$capture_pid" || true
capture_pid=

cp "$scratch/$name.pcap" "$data/$name.pcap"
tables "$data/$name.pcap" "$data/$name"
echo "capture_rtp.sh: wrote $data/$name.{pcap,rtp.tsv,rtcp.tsv}"
