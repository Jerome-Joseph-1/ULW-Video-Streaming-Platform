// Decodes each input twice, in one write and in writes of fuzzer-chosen sizes, and requires the
// same frames and the same error from both. Every delivered frame must satisfy what the decoder
// promises its caller, must be accepted by the server encoder unchanged, and must come back
// identical from a client encoding of it.
//
// Input: byte 0 picks the write size, byte 1 the message limit and the size jitter; the rest
// is the stream.

#include "codec/ws/decoder.hpp"
#include "codec/ws/encoder.hpp"
#include "codec/ws/frame.hpp"
#include "codec/ws/utf8.hpp"
#include "core/ports/random.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

void check(bool invariant) {
    if (!invariant) {
        __builtin_trap();
    }
}

// Below, at and above the 125-byte control limit and the 16-bit length form, and the default.
constexpr std::array<std::uint64_t, 4> kLimits{0, 200, 70'000,
                                               codec::ws::Decoder::kDefaultMaxMessageBytes};

class CountingKeys final : public core::ports::IRandom {
public:
    void fill(std::span<std::byte> out) noexcept override {
        for (std::byte& b : out) {
            b = static_cast<std::byte>(next_++);
        }
    }

private:
    std::uint8_t next_ = 0x5A;
};

codec::ws::Decoded decode(std::span<const std::byte> stream, std::uint64_t limit,
                          std::size_t write_size, std::uint8_t jitter) {
    codec::ws::Decoder decoder{limit};
    codec::ws::Decoded all;
    std::size_t offset = 0;
    std::uint8_t turn = 0;
    while (offset < stream.size()) {
        // Vary the size a little per write, so a fixed size does not always cut in the same
        // place relative to each frame.
        const std::size_t size = write_size + ((jitter * ++turn) & 7U);
        const std::size_t n = std::min(size, stream.size() - offset);
        codec::ws::Decoded d = decoder.feed(stream.subspan(offset, n));
        offset += n;
        std::ranges::move(d.frames, std::back_inserter(all.frames));
        if (d.error) {
            all.error = d.error;
            check(decoder.error() == d.error);
            check(decoder.feed(stream.first(std::min<std::size_t>(stream.size(), 4))).error ==
                  d.error);
            break;
        }
    }
    return all;
}

void check_frame(const codec::ws::Frame& f, std::uint64_t limit) {
    check(f.fin);
    check(f.opcode != codec::ws::Opcode::Continuation);
    if (codec::ws::is_control(f.opcode)) {
        check(f.payload.size() <= 125);
    } else {
        check(f.payload.size() <= limit);
    }
    if (f.opcode == codec::ws::Opcode::Text || f.opcode == codec::ws::Opcode::Close) {
        check(codec::ws::is_valid_utf8(f.payload));
    }
    if (f.opcode == codec::ws::Opcode::Close) {
        check(f.close_code == codec::ws::CloseCode::NoStatus ||
              codec::ws::is_valid_on_wire(f.close_code));
    } else {
        check(f.close_code == codec::ws::CloseCode::NoStatus);
    }

    std::vector<std::byte> echoed;
    check(codec::ws::encode(f, echoed).has_value());

    CountingKeys keys;
    codec::ws::ClientEncoder client{keys};
    std::vector<std::byte> wire;
    check(client.encode(f, wire).has_value());
    codec::ws::Decoder again{limit};
    const codec::ws::Decoded d = again.feed(wire);
    check(!d.error && d.frames.size() == 1 && d.frames[0] == f);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t kControlBytes = 2;
    if (size < kControlBytes) {
        return 0;
    }
    const std::span<const std::byte> input = std::as_bytes(std::span{data, size});
    const std::size_t write_size = std::to_integer<std::size_t>(input[0]) + 1;
    const auto control = std::to_integer<std::uint8_t>(input[1]);
    const std::uint64_t limit = kLimits.at(control & 3U);
    const auto jitter = static_cast<std::uint8_t>(control >> 2U);
    const std::span<const std::byte> stream = input.subspan(kControlBytes);

    const codec::ws::Decoded whole = decode(stream, limit, stream.size() + 1, 0);
    const codec::ws::Decoded split = decode(stream, limit, write_size, jitter);

    check(whole.error == split.error);
    check(whole.frames == split.frames);
    for (const codec::ws::Frame& f : whole.frames) {
        check_frame(f, limit);
    }
    return 0;
}
