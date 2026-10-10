# 0016. End-to-end encryption for private chat only

Status: Accepted, amended by 0102 (multi-device: every device is its own member, found through the key directory)
Date: 2026-09-28

## Context

The realtime plane carries 1:1 and small-group chat, calls, live streams and the live chat beside
a stream. Private chat should not be readable by the operator. Calls and streams must be
moderated and can be recorded, and recordings feed the VOD pipeline. Live chat is public to the
stream's audience. Clients are browsers only.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| E2EE everywhere, calls included (SFrame, insertable streams) | One privacy story; the server never sees content | Rejected: the server could no longer moderate or record calls and streams |
| Signal protocol through libsignal | Proven for 1:1 and widely deployed | Rejected: AGPL-3.0, and groups are built from pairwise sessions or sender keys rather than a standard group protocol |
| No E2EE; chat protected by TLS only | Server-side search, moderation and easy multi-device | Rejected: the operator could read private conversations |
| MLS (RFC 9420) via OpenMLS for 1:1 and small groups; transport encryption elsewhere | A standard group protocol with forward secrecy and post-compromise security; 1:1 is a two-member group | Accepted |

## Decision

- 1:1 and small-group chat are end-to-end encrypted with MLS (RFC 9420), implemented with
  OpenMLS on the client, compiled to WebAssembly for the browser. A 1:1 conversation is a
  two-member group. The server stores and forwards MLS messages and orders commits (ADR-0015); it
  holds no group secrets.
- Calls, live streams and live chat are encrypted in transport only, TLS for signalling and chat
  and DTLS-SRTP for media, so the server can moderate and record them.
- Message bodies are opaque `bytea` end to end: never parsed, logged, indexed or moderated on
  content.

## Consequences

- Metadata stays visible to the server: sender, room, time and size.
- Private chat has no server-side search, and moderation there is limited to metadata and what
  members report.
- Known gap: multi-device key management. Each device is its own MLS member, a new device cannot
  read messages from before it joined, and adding one needs a commit from a device already in the
  group. This is not designed yet.
- Monitor decryption failures that clients report; epoch mismatches point at commit-ordering
  bugs on the server.
- Reopen when multi-device is designed, or if a call product appears that does not need
  recording and wants E2EE.
