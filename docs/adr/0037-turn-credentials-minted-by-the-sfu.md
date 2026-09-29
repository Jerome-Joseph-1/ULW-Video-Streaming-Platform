# 0037. STUNner runs in front of LiveKit with time-windowed credentials the SFU mints

Status: Accepted
Date: 2026-09-29
Amends: ADR-0013's "Long-term credentials are static" (the credentials are now time-windowed)

## Context

ADR-0013 puts STUNner, a TURN server run by a Gateway API operator, in front of the SFU, with
"TURN long-term credentials" from a Secret. That phrase covers two different STUNner modes, both
of which use the long-term credential mechanism of RFC 5389 on the wire: `static` (one username
and password for every client) and `ephemeral`, which STUNner's older documentation and the brief
call `longterm` (a shared secret; each credential is `<expiry>:<user>` with the base64
HMAC-SHA1 of that under the secret as its password, the "TURN REST API" draft). Someone has to
hand each browser its credential, and the call must survive a relay that only lets calls in.

Three more choices shape what can be tested: where the SFU runs in the sandbox, how a client
"outside the cluster network" is simulated, and how STUNner is installed without Helm (its chart
repository, l7mp.io, is unreachable from the build machines; GitHub is not).

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| `static` credentials, one pair in the page or the ticket | Simplest; the STUNner quick start | Rejected: a pair that leaks works for everyone until someone rotates it, which breaks every client at once |
| `ephemeral`, minted by the call handler and sent next to the ticket (the plan of the ADR that gives clients a ticket to the SFU) | Our code decides lifetime and identity | Rejected for now: a second secret in the call service, a second field in the ticket and client code to pass ICE servers, for what LiveKit already does |
| `ephemeral`, minted by LiveKit: its `rtc.turn_servers` entry holds STUNner's address and the shared secret, and LiveKit sends each participant a fresh credential in its join response | No code of ours at all; the credential exists only for a client that got in with a valid ticket; the LiveKit SDK uses it without being told | Accepted |
| LiveKit outside the cluster (the call suite's compose server) with STUNner routing to it through a `StaticService` | Reuses the local server | Rejected: the point of M26 is the cluster path; STUNner resolves UDPRoute backends from EndpointSlices, which only an in-cluster Service has |
| Client "outside" = the host itself, or a Docker network with masquerading left on | No extra network | Rejected: the host reaches the node through its own loopback DNAT, and a masqueraded network hides the client's address, so XOR-MAPPED-ADDRESS would equal the address check whether or not the path preserves sources |
| Client outside = a Docker network in routed mode (no NAT), reaching the node through ports kind publishes on that network's gateway address | Same DNAT path as a browser reaching k8s-prod's node; the client keeps its address; no root needed | Accepted |
| Install STUNner with `helm install` from its repository | The documented way | Rejected: the repository is unreachable here and Helm would be another pinned tool; the chart's three templates render to plain manifests, and its CRD file is fetched by commit and SHA-256 like every other download |

## Decision

- STUNner v1.2.1: the operator (the chart's templates rendered into `deploy/askedin/stunner/`,
  images pinned by digest), CRDs from the chart at tag v1.2.1 by SHA-256. The Gateway API CRDs
  stay Envoy Gateway's.
- `authType: ephemeral`, shared secret in Secret `stunner-secrets`. LiveKit gets the same secret
  and STUNner's address, and mints credentials with a 4 h lifetime.
- STUNner is cluster-wide, so there is one TURN secret per cluster, and LiveKit in every
  environment reads that same value from its own `sfu-secrets`. `stunner-secrets` is created
  by a step of its own, once per cluster, which refuses when the stage and prod env files
  disagree; it carries no `ASKEDIN_ENV`, since it belongs to neither.
- One Gateway, one listener, `TURN-UDP` on 3478, `externalTrafficPolicy: Local`; a UDPRoute to
  LiveKit's single UDP port (7882). LiveKit advertises only its pod address and has ICE over TCP
  off, so the relay is the only path media can take.
- In the sandbox LiveKit runs in the kind cluster from the same stage manifests, and the only UDP
  port published on the host is 3478, on the outside network's gateway address 198.18.0.1.

## Consequences

- Clients need no TURN code: ICE servers arrive in LiveKit's join response. Whoever holds a
  ticket gets a credential; a leaked credential reaches only LiveKit's UDP port and dies within
  4 h. A call longer than that loses its relay at the next allocation refresh and the SDK
  reconnects for a fresh one; watch reconnects by call age if long calls become common.
- The ticket ADR's "the call handler sends the per-session TURN credentials next to the ticket"
  is not needed; the call handler only sends the ticket.
- Rotating the secret means updating both Secrets and restarting LiveKit; credentials minted
  under the old secret fail from then on, so clients mid-call reconnect once.
- STUNner v1.2.1 (pion/turn) answers a bad MESSAGE-INTEGRITY and an expired credential with 400,
  and a refused permission with an error carrying no ERROR-CODE; RFC 8656 asks for 401 and 403.
  Clients only look at the class, but a monitor that counts 401s would see none.
- Stage and prod share the GatewayClass and its secret; a stage credential passes
  authentication on the prod listener once prod has one, reaching only prod LiveKit's UDP
  port. Split the GatewayConfig, and the secret with it, per environment if that matters.
- The secret sits in two env files that must agree. The per-cluster step checks that, but
  only when someone runs it: a `TURN_SECRET` changed in one file and pushed with
  `create-k8s-secrets.sh` alone breaks that environment's relay until the step runs.
- Docker 27 or later is needed for the sandbox's routed network.
- Reopen if the call service needs to decide who may relay independently of LiveKit (for
  example, TURN for a peer-to-peer path that bypasses the SFU), or if STUNner's free tier stops
  covering what we use.
