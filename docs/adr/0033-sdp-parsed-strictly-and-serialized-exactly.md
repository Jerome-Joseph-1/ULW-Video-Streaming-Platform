# 0033. SDP is parsed strictly into a typed model that serializes back exactly

Status: Accepted
Date: 2026-09-29

## Context

Calls negotiate through `IMediaParticipant::apply_offer(sdp) -> answer` (ADR-0020): the
browser's offer arrives over the room WebSocket, is checked at the signalling boundary, goes
to the SFU, and the SFU's answer goes back. `codec/sdp` is the check. The text is untrusted,
up to the 64 KiB a WebSocket message may carry (ADR-0029), and the browser must accept whatever
we send back, so a description that goes through the codec must come out unchanged: M24 asks
that a real Chromium offer round-trip parse -> serialize and still be accepted by Chromium.

RFC 8866 and the attribute RFCs allow more than any browser sends: leading zeros in numbers,
literal keywords in any case, several spellings of the same attribute. A model that accepts
two spellings of one thing cannot write both back.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Treat SDP as lines and edit them with string operations ("SDP munging") | No model to maintain; what WebRTC applications commonly do | Rejected: nothing checks the offer, and every edit is a new place to break the grammar |
| A third-party parser (libwebrtc's, sofia-sip, sdp-transform) | Mature, widely deployed | Rejected: libwebrtc's parser comes with libwebrtc; sofia-sip brings its own allocator and event loop; sdp-transform is JavaScript; none validates what our boundary needs, and none promises an exact round trip |
| A typed model with a lenient grammar and a canonical serializer | Accepts everything any peer might send | Rejected: an accepted description no longer serializes to itself, so "byte for byte" holds only for inputs that were already canonical, and nobody can tell which those are |
| A typed model with a strict grammar wherever exactness needs it, unknown attributes kept verbatim | Every accepted description serializes to itself; the model is comparable and can be built by hand for answers | Accepted |

## Decision

- `codec::sdp::parse(text, limits)` returns a `Session` or an `Error { code, line }`. Every
  string in the model is a `string_view` into the text (ADR-0017). `serialize(session)`
  writes the lines back in RFC 8866 order, each ended by CRLF.
- The promise, checked by the fuzz target on every input it accepts: `serialize(parse(x))`
  is `x` with every line ended by CRLF. The grammar is narrowed exactly where that needs it:
  - numbers have no leading zero ("0" itself excepted), since `09` and `9` read the same;
  - fields are separated by single spaces, with none leading or trailing;
  - literal keywords (`typ`, `raddr`, `send`, `actpass`, directions) are accepted in the case
    every implementation sends them, lower case;
  - line order is RFC 8866 section 5's, and a line type the parser does not know fails the
    whole description, as that section requires.
- WebRTC's attributes are typed: rtpmap, fmtp, rtcp-fb, extmap, mid, group, msid, ssrc,
  ssrc-group, ice-ufrag, ice-pwd, ice-options, candidate, end-of-candidates, fingerprint,
  setup, rtcp-mux, rtcp-rsize, the four directions, rid and simulcast. Free-form tails (fmtp
  parameters, candidate extensions, rid restrictions, extmap attributes) stay verbatim. Any
  other attribute is kept as its name and value, unparsed.
- The boundary enforces, beyond the grammar: every section has exactly one mid, unique across
  the description; BUNDLE groups name existing mids, each at most once, and their tagged
  section is neither rejected nor bundle-only; a bundle-only section is in a group; ICE
  credentials, a fingerprint and a setup role describe every section that needs its own
  transport (outside a group, or tagged); fingerprints use SHA-256, SHA-384 or SHA-512, with a
  digest of that length; rtpmap, fmtp and rtcp-fb name formats of their m= line, and a payload
  type is mapped once; extmap ids are unique across session and section; simulcast names only
  rids declared with the same direction.
- Limits are parameters with derived defaults: 58 KiB, 32 media sections, 256 attributes per
  level; 128 formats per m= line is fixed by the payload type space.

## Consequences

- Valid but unusual SDP is refused: `a=setup:ACTIVE`, a number with a leading zero, two spaces
  between fields, a blank line, an SHA-1 fingerprint. Chromium 141's offers and answers pass
  (tests/data/sdp); Firefox and Safari have not been captured yet, and a refusal from either
  is the first thing to check if a call fails to start from them.
- A refusal says which line and which rule, so a client bug is diagnosable from the log.
- The answer M25 sends is a `Session` built by the SFU adapter, with views into strings it
  owns, and checked by parsing its own serialization in tests.
- A hand-built `Candidate` whose extensions start with `raddr` or `rport` would read back
  as those fields; the model does not prevent it. Parsed candidates cannot have that shape.
- Reopen if a browser we support sends SDP that this grammar refuses and cannot be narrowed
  to accept without giving up the round trip.
