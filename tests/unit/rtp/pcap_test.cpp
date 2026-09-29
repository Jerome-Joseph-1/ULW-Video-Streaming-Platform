// M24 acceptance: on a capture of ffmpeg's RTP muxer, the parser reads the sequence number,
// timestamp and SSRC that tshark reads from every RTP packet, sorts RTP from RTCP on the shared
// port as tshark does, and reads the same sender reports. tests/media/capture_rtp.sh makes the
// capture and tshark's tables, and checks the committed tables against tshark again.

#include "codec/rtp/demux.hpp"
#include "codec/rtp/rtcp.hpp"
#include "codec/rtp/rtp.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using codec::rtp::classify;
using codec::rtp::CompoundReader;
using codec::rtp::PacketKind;
using codec::rtp::parse_rtp;
using codec::rtp::RtcpPacket;
using codec::rtp::SenderReport;

const std::filesystem::path kData{ULW_RTP_DATA_DIR};
constexpr std::string_view kCapture = "ffmpeg_vp8_opus";

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<std::byte> out;
    char c = 0;
    while (in.get(c)) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::uint32_t le32(std::span<const std::byte> b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 4; i > 0; --i) {
        v = (v << 8U) | std::to_integer<std::uint32_t>(b[at + i - 1]);
    }
    return v;
}

std::uint16_t be16(std::span<const std::byte> b, std::size_t at) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(b[at]) << 8U) |
                                      std::to_integer<unsigned>(b[at + 1]));
}

// The UDP payload of each frame of a classic pcap file of Ethernet frames, which is what
// tcpdump -w writes on lo. Frame numbers are 1-based, as tshark counts them.
std::vector<std::vector<std::byte>> udp_payloads(std::span<const std::byte> file) {
    constexpr std::size_t kFileHeader = 24;
    constexpr std::size_t kRecordHeader = 16;
    constexpr std::uint32_t kMicrosecondMagic = 0xA1B2C3D4;
    constexpr std::uint32_t kEthernet = 1;
    constexpr std::size_t kEthernetHeader = 14;
    constexpr std::uint16_t kIpv4 = 0x0800;
    constexpr std::uint8_t kUdp = 17;
    constexpr std::size_t kUdpHeader = 8;
    std::vector<std::vector<std::byte>> out;
    if (file.size() < kFileHeader || le32(file, 0) != kMicrosecondMagic ||
        le32(file, 20) != kEthernet) {
        ADD_FAILURE() << "not a microsecond pcap of Ethernet frames";
        return out;
    }
    std::size_t at = kFileHeader;
    while (at + kRecordHeader <= file.size()) {
        const std::uint32_t captured = le32(file, at + 8);
        const std::span<const std::byte> frame = file.subspan(at + kRecordHeader, captured);
        at += kRecordHeader + captured;
        EXPECT_EQ(be16(frame, 12), kIpv4);
        const std::span<const std::byte> ip = frame.subspan(kEthernetHeader);
        const std::size_t ip_header = 4 * (std::to_integer<std::size_t>(ip[0]) & 0x0FU);
        EXPECT_EQ(std::to_integer<std::uint8_t>(ip[9]), kUdp);
        const std::span<const std::byte> udp = ip.subspan(ip_header);
        const std::span<const std::byte> payload =
            udp.subspan(kUdpHeader, be16(udp, 4) - kUdpHeader);
        out.emplace_back(payload.begin(), payload.end());
    }
    EXPECT_EQ(at, file.size());
    return out;
}

// tshark -T fields -E header=y: a header line, then tab-separated fields.
std::vector<std::vector<std::string>> read_table(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::vector<std::vector<std::string>> rows;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        std::string_view rest = line;
        while (true) {
            const std::size_t tab = rest.find('\t');
            fields.emplace_back(rest.substr(0, tab));
            if (tab == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(tab + 1);
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

template <typename T> T number(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
        base = 16;
    }
    T value{};
    const char* const first = text.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char* const last = first + text.size();
    const auto [end, ec] = std::from_chars(first, last, value, base);
    EXPECT_TRUE(ec == std::errc{} && end == last) << text;
    return value;
}

class Capture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const std::string name{kCapture};
        file_ = read_file(kData / (name + ".pcap"));
        payloads_ = udp_payloads(file_);
        rtp_ = read_table(kData / (name + ".rtp.tsv"));
        rtcp_ = read_table(kData / (name + ".rtcp.tsv"));
    }

    static const std::vector<std::byte>& frame(const std::string& number_text) {
        return payloads_.at(number<std::size_t>(number_text) - 1);
    }

    static inline std::vector<std::byte> file_;
    static inline std::vector<std::vector<std::byte>> payloads_;
    static inline std::vector<std::vector<std::string>> rtp_;
    static inline std::vector<std::vector<std::string>> rtcp_;
};

TEST_F(Capture, TsharkTablesAccountForEveryPacketOnce) {
    // Eight seconds of 15 fps VP8 and 20 ms Opus frames: several hundred packets.
    ASSERT_GT(payloads_.size(), 400U);
    std::set<std::string> frames;
    for (const auto& row : rtp_) {
        EXPECT_TRUE(frames.insert(row.at(0)).second) << row.at(0);
    }
    for (const auto& row : rtcp_) {
        EXPECT_TRUE(frames.insert(row.at(0)).second) << row.at(0);
    }
    EXPECT_EQ(frames.size(), payloads_.size());
}

TEST_F(Capture, EveryRtpPacketReadsAsTsharkReadsIt) {
    std::set<std::uint32_t> ssrcs;
    for (const auto& row : rtp_) {
        const auto& payload = frame(row.at(0));
        ASSERT_EQ(classify(payload), PacketKind::Rtp) << "frame " << row[0];
        const auto p = parse_rtp(payload);
        ASSERT_TRUE(p.has_value()) << "frame " << row[0];
        EXPECT_EQ(p->sequence, number<std::uint16_t>(row.at(1))) << "frame " << row[0];
        EXPECT_EQ(p->timestamp, number<std::uint32_t>(row.at(2))) << "frame " << row[0];
        EXPECT_EQ(p->ssrc, number<std::uint32_t>(row.at(3))) << "frame " << row[0];
        EXPECT_EQ(p->payload_type, number<std::uint8_t>(row.at(4))) << "frame " << row[0];
        EXPECT_EQ(p->marker, row.at(5) == "True") << "frame " << row[0];
        ssrcs.insert(p->ssrc);
    }
    // One stream of VP8 and one of Opus.
    EXPECT_EQ(ssrcs.size(), 2U);
}

TEST_F(Capture, EveryRtcpPacketReadsAsTsharkReadsIt) {
    ASSERT_FALSE(rtcp_.empty());
    for (const auto& row : rtcp_) {
        const auto& payload = frame(row.at(0));
        ASSERT_EQ(classify(payload), PacketKind::Rtcp) << "frame " << row[0];
        std::vector<RtcpPacket> packets;
        CompoundReader reader{payload};
        while (auto next = reader.next()) {
            ASSERT_TRUE(next->has_value()) << "frame " << row[0];
            packets.push_back(**next);
        }
        // ffmpeg sends each sender report on its own; tshark lists one type per packet.
        ASSERT_EQ(packets.size(), 1U) << "frame " << row[0];
        ASSERT_EQ(row.at(1), "200") << "frame " << row[0];
        const auto& sr = std::get<SenderReport>(packets[0]);
        EXPECT_EQ(sr.ssrc, number<std::uint32_t>(row.at(2))) << "frame " << row[0];
        EXPECT_EQ(sr.rtp_timestamp, number<std::uint32_t>(row.at(3))) << "frame " << row[0];
        EXPECT_EQ(sr.packet_count, number<std::uint32_t>(row.at(4))) << "frame " << row[0];
        EXPECT_EQ(sr.octet_count, number<std::uint32_t>(row.at(5))) << "frame " << row[0];
    }
}

} // namespace
