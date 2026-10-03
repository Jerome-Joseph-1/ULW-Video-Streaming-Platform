# 0097. Incoming calls ring offline browsers through standard Web Push, sent by the room's owner

Status: Accepted
Date: 2026-10-03
Amends: ADR-0091 (a member with no socket open is rung by push)

## Context

ADR-0091 rings a callee on every socket they have open, on any node, and stops there: "Offline
members hear nothing ... push notifications are out of scope". A web app in a closed or
discarded tab, or a phone's browser with the app in the background, has no socket, so the callee
never knows they were called.

What a push needs, and what bounds it:

- **No money and no paid service.** The browsers' own push services (Chrome's and the other
  Chromium browsers' FCM endpoint, Mozilla's autopush, Apple's Web Push, Microsoft's WNS) take
  standard Web Push (RFC 8030) from any application server, free, with no account: the sender
  identifies itself with a key pair of its own (VAPID, RFC 8292), and encrypts each message to
  the browser (RFC 8291). Native mobile push (FCM to an Android app, APNs to an iOS app) needs a
  developer account, a provider credential and an app; there is no app yet.
- **Where the ring is.** The room's owner decides that a call starts ringing (ADR-0091); it is
  the one place that knows the callees and the call.
- **The endpoint is a URL a client chose.** Every push is an HTTPS POST from inside the cluster
  to it: a server-side request forgery surface (to the cloud metadata address, to Postgres, to
  anything on the pod network) unless the destination is constrained.
- **The reactor.** No blocking: the POSTs, the Postgres reads and the crypto all happen on the
  loop, bounded (ADR-0036).
- **Privacy.** The push service sees the endpoint, the timing and the ciphertext's size; it must
  not see who calls whom, or anything of a conversation.

## Options

| Question | Option | Verdict |
|---|---|---|
| Which push | FCM's HTTP v1 API, APNs | Rejected for now: a provider account and credentials, and native apps that do not exist. A later option (Consequences) |
| | A third-party push SaaS (OneSignal, Pusher Beams) | Rejected: a paid tier and a dependency the brief rules out |
| | Standard Web Push, VAPID-signed, aes128gcm | Accepted: free, the same protocol for every browser, no account |
| Encryption and signing | A Web Push library (C, Rust, Node) | Rejected: a new dependency for some 300 lines over OpenSSL, which is in the tree and audited already |
| | OpenSSL 3's EVP: P-256 ECDH, HMAC-SHA-256 for HKDF, AES-128-GCM, ECDSA for ES256 | Accepted: tested against RFC 8291's Appendix A vector byte for byte |
| Who is pushed | Only callees with no socket open anywhere | Rejected: the owner does not know where sockets are without a presence lookup per callee, and an open socket is no sign of a screen anyone looks at (a background tab is connected) |
| | Every subscribed device of every callee, once, as the ring starts; clients deduplicate by call id | Accepted: one read per callee; the notification is tagged with the call so the browser shows it once |
| Pushes for answered, declined, cancelled, missed | A second push that closes the notification | Rejected: browsers require every push to show a notification (`userVisibleOnly`), so a closing push would show one; an open page closes it on the socket's event, and the click checks `expires_at` |
| Payload | The call_ringing event the socket carries: room, call, caller, expires_at | Accepted: nothing of any message, nothing the client cannot already know; one code path in the client |
| How clients register | A new authenticated HTTP endpoint on chat | Rejected: chat serves WebSockets only; a second authenticated surface is a second place to get auth wrong |
| | Commands on the chat socket: `push_key`, `push_subscribe`, `push_unsubscribe` | Accepted: the socket is already authenticated, rate-limited and documented |
| Where subscriptions live | Per user, in memory on some node | Rejected: the ring's owner is any node |
| | Postgres, one row per (user, device), endpoint unique (migration 0016) | Accepted: the owner reads a callee's handful of rows by key |
| Endpoint SSRF | An allowlist of hosts alone | Rejected: a name on the list can still resolve to a private address (a DNS change, a hijacked record) |
| | Resolve and check before connecting | Rejected: the connect resolves again, and the answer may differ (DNS rebinding) |
| | Validate at subscribe and at send (https, 443, length, a name not an address, an operator allowlist of hosts), and check every address libcurl is about to connect to, in libcurl's open-socket callback; no proxy, no redirects | Accepted: the check is on the address actually connected to |
| Retries | Any failure | Rejected: the brief, and a POST that may have arrived; a network failure is counted |
| | 5xx and 429 only, after Retry-After (delta-seconds) or a doubling backoff, three attempts, never past the ring's end | Accepted |
| Sending | A thread pool of blocking requests | Rejected: the reactor already drives libcurl (`infra::curl::Multi`) |
| | libcurl multi on the reactor, its own multi with 16 connections, a queue of 1024 | Accepted |

## Decision

- **Protocol.** Web Push as RFC 8030 (POST to the endpoint, `TTL`, `Urgency: high`,
  `Content-Encoding: aes128gcm`), RFC 8291 encryption (a fresh P-256 key and salt per message,
  one 4096-byte record), RFC 8292 VAPID (`Authorization: vapid t=<ES256 JWT>, k=<public key>`,
  `aud` the endpoint's origin, `exp` 12 h ahead, `sub` the operator's contact; one header per
  push service, signed again an hour before it expires). Every plaintext is padded to a multiple
  of 512 bytes, so a push's length does not tell callers or rooms apart. All on OpenSSL
  (`infra/webpush`); the encryption reproduces RFC 8291's example exactly (its section 5 request
  says `Content-Length: 145`; the body it shows is 144 bytes, which is what the vector checks).
- **The key** is the operator's: `ULW_PUSH_VAPID_PRIVATE_KEY` in chat's Secret, a P-256 scalar in
  base64url, checked at start (exit 2, never quoted, never logged). Without it push is off and
  the commands answer `push_disabled`. Clients get its public half from `push_key`.
- **Subscriptions** (`push_subscriptions`, migration 0016) are keyed by (user, device), the
  endpoint unique. Saving is one function call (`push_subscribe`) under a per-user advisory
  lock: it takes the endpoint from whoever held it, writes the row and forgets the user's
  devices saved longest ago past the cap (10, `ULW_PUSH_MAX_SUBSCRIPTIONS_PER_USER`, 1 to 32).
  A 404 or 410 from the push service deletes the row.
- **Validation.** At subscribe and again before each send: `https://`, port 443, at most 2048
  bytes, printable ASCII without userinfo or fragment, a host name (an IP literal in any form,
  `127.1` and `0x7f.1` included, is refused), and a host on the operator's list (`ULW_PUSH_HOSTS`,
  exact names or `*.domain`; by default `fcm.googleapis.com`, `updates.push.services.mozilla.com`,
  `web.push.apple.com`, `*.notify.windows.com`). The p256dh key must be a point on P-256. At
  connect time, every address the name resolved to must be global unicast (`net::is_global_unicast`:
  no RFC 1918, loopback, link-local, CGNAT, multicast, documentation, benchmarking, reserved,
  unique local, NAT64, 6to4, Teredo); libcurl is given no proxy, follows no redirect and speaks
  https only, with the TLS 1.3 floor every outbound call has (operator-contract.md).
- **Sending.** `Ringer::start` hands each new ring to the owner's `Push`, which reads each
  callee's subscriptions (256 reads waiting at most), encrypts the call_ringing event to each,
  and queues it on the `PushSender`: 1024 messages waiting, 16 POSTs at once on a libcurl multi
  of its own, each attempt's TTL what is left of the ring. Retries only for 5xx and 429, after
  `Retry-After` (more than a day: no retry) or a backoff of 1 s, 2 s, ... less a random half,
  three attempts, never past the ring's end; the sender's retries
  run from the server's per-turn sweep, on the injected clock.
- **Bounds and rates.** Subscribes and unsubscribes are 5 at once per socket, then one each
  10 s, with 4 store writes waiting per user and 64 node-wide (`busy` past either). An endpoint
  is stored canonical (scheme and host in lower case, no default port), so one endpoint is one
  row whatever its spelling.
- **Metrics** with fixed labels: `push_subscriptions_total{op}`, `push_lookups_total{outcome}`,
  `push_messages_total{outcome}`, `push_sends_total{outcome}` (delivered, gone, rejected,
  failed, refused_address, expired, dropped), `push_retries_total`, `push_queue_depth`,
  `push_in_flight`, `push_store_failures_total`, `push_enabled`. No endpoint, key or user in a
  label or a log line.
- **Operator surface.** The key and the subject in `CHAT_SECRET`, both optional, so an
  environment without push changes nothing; the host list is chat's default, which an overlay
  may override by patching `ULW_PUSH_HOSTS`; egress on 443 (already open for the JWKS). Development only, behind
  `ULW_DEV_MODE=1` outside a pod: `ULW_DEV_PUSH_ALLOW_PRIVATE` (a loopback push service on any
  port) and `ULW_DEV_PUSH_CA_FILE`, which the integration test uses.

## Consequences

- A callee with a subscribed browser is rung with no socket open, at the cost of one indexed
  read per callee per ring and one POST per device. No message content ever leaves for a push
  service; the push service learns that a device was pushed to, when, and the size.
- The push is not withdrawn: a device that was offline sees the notification until it is
  clicked or the app opens and closes it, and the click sees `expires_at` passed and offers to
  call back (calls.md).
- Rotating the VAPID key orphans every subscription until its client next opens and
  re-subscribes (clients compare `push_key` with the key they subscribed with). The push
  services answer such pushes 401 or 403, counted as `rejected`.
- A push service that offers only TLS 1.2 is refused like any other outbound HTTPS peer
  (`failed`); the floor is not lowered for one. The four default services, Edge's WNS
  (`*.notify.windows.com`) included, negotiated TLS 1.3 when checked on 2026-10-03
  (`openssl s_client -tls1_3`, the services' own certificates).
- Pushes go from the room's owner, so every chat pod needs the egress. An owner change mid-ring
  sends nothing more: the push already left at the ring's start.
- Direct messages are not pushed: their bodies are end-to-end encrypted or opaque to the server
  (ADR-0016), so a push could only say "a new message", and would need per-room rate limits and
  mute settings; reopen with a product decision on what such a notification says.
- The migration is numbered 0016, after 0011 to 0015 of work landing alongside it: migrations
  run without gaps and only forward, so it merges after those, never before.
- **Later options.** Native push for apps: FCM HTTP v1 (an OAuth service account per project)
  and APNs (a .p8 token key per team), each as another sender behind the same `Push`, with a
  `platform` on the subscription row; both free to use but needing provider accounts. Pushing
  call endings could be added for platforms that allow a silent push (APNs background, FCM data
  messages to an app).
