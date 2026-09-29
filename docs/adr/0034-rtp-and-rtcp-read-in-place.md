# 0034. RTP and RTCP are read in place, for tooling and tests

Status: Accepted
Date: 2026-09-29

## Context

Media belongs to the SFU (ADR-0020): ULW never decrypts, forwards or answers RTP. It still
needs to read RTP and RTCP: the media test harness and the RTP tooling built on the datagram
reactor (M23), and M24's acceptance check that the parser agrees with tshark on a capture. The
input is a datagram from a socket or a pcap, possibly hostile, and the reader has to hold no
state between datagrams.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Wireshark's dissectors or libpcap-based tooling | Complete, and tshark is already the reference | Rejected: GPL and a large dependency for a check we can make against tshark's output instead |
| Decode each packet into owning structs (vectors of report blocks, CSRCs, SDES items) | Easy to consume | Rejected: an allocation per packet and per list, where every list has a fixed-size entry that can be read where it lies |
| Views into the datagram: fixed fields decoded, lists as `Entries<T>` decoded on access, variable-length structures validated whole before they are handed out | No allocation, bounded work per datagram, iteration that cannot fail | Accepted |

## Decision

- `parse_rtp()` reads RFC 3550's header, CSRCs, padding, and the header extension, whose
  RFC 8285 one-byte and two-byte elements are walked once for validity and then iterated by
  `ExtensionElements`. ID 0 is padding and one-byte ID 15 ends the extension, as RFC 8285
  section 4.2 says.
- `CompoundReader` yields the packets of an RTCP datagram: SR, RR, SDES, BYE, APP, the generic
  NACK and transport-wide congestion control feedback (its fixed fields; the status chunks stay
  encoded), PLI, FIR and REMB. Other feedback formats and packet types come through untyped
  with their bytes. It does not require a report first: RFC 5506 reduced-size RTCP, which
  browsers negotiate, sends feedback alone. Only the last packet may be padded (RFC 3550
  section 6.4.1); SDES chunks must add up to the packet exactly; a REMB bitrate that does not
  fit 64 bits is refused.
- `classify()` separates RTP from RTCP on one port by the second byte (RFC 5761 section 4) and
  calls anything that is not version 2, STUN and DTLS included (RFC 7983), Other.
- SRTP and SRTCP are not decrypted. RTP headers stay readable under SRTP, RTCP bodies do not;
  the pcap fixture is therefore plain RTP from ffmpeg's muxer, RTCP multiplexed on the RTP port.

## Consequences

- Every span in a packet lies inside the datagram it came from; the fuzz target checks that,
  and that an RTP packet's parts add up to the datagram's length.
- Packets are only as valid as their bytes: nothing checks sequence continuity, SSRC
  consistency across a compound, or that the first RTCP packet reports.
- No serializer: nothing in ULW sends RTP. Tests build packets byte by byte.
- Reopen if ULW ever terminates media itself (a recording or live ingest path outside the SFU),
  which would need SRTP and a writer.
