# 0109. An external TURN service as an alternative to STUNner, with credentials in each ticket

Status: Accepted
Date: 2026-10-14
Amends: ADR-0037 ("Clients need no TURN code: ICE servers arrive in LiveKit's join response"
holds only in the self-hosted mode, still the default)

## Context

The realtime plane relays every call through STUNner, a TURN server the cluster runs as a
LoadBalancer on a UDP port open to the whole internet (ADR-0013, ADR-0037, ADR-0084). Askedin runs
ULW on one node that also serves its website, behind Cloudflare's free plan, which does not proxy
UDP. Their audit of v1.0.1 found the plane sound and named one remaining exposure: a UDP flood at
the TURN port lands on the box that serves the site, and nothing in front of it absorbs it. They
asked for an opt-in mode in which a managed TURN service (Cloudflare Realtime TURN, or any TURN
server speaking the TURN REST API's credentials) relays the media instead, with the self-hosted
mode unchanged as the default, and for an answer to one question: does LiveKit then reach clients
through the external relay with no inbound port at all?

What we found, from LiveKit v1.13.7's source (`pkg/service/roommanager.go`,
`iceServersForParticipant`; `mediatransportutil/pkg/rtcconfig/webrtc_config.go`):

- **LiveKit is never a TURN client.** `rtc.turn_servers` only fills the ICE servers it sends
  clients; its own candidates are host candidates on its UDP port (or port range), rewritten to
  `node_ip` when one is set. Nothing makes it allocate a relay of its own.
- So the provider's relay must reach LiveKit's host candidate: **one inbound UDP port stays**, on
  an address the relay can route to. Cloudflare refuses `CreatePermission` for private addresses,
  so that is the node's public address, not the pod's.
- **The node's address is not hidden.** A browser's relay needs a permission for LiveKit's
  address before it forwards anything, so LiveKit sends its candidate, the node's public address,
  to every participant over signalling. Any participant with a ticket learns it, as they learn
  STUNner's address (`TURN_HOST`) today.
- A stateful firewall that admits no inbound packet at all would still let a call through when
  LiveKit's own connectivity check opens the path first, as ICE does through a NAT. That holds
  only while the node's masquerade keeps LiveKit's source port; with port randomisation the check
  arrives as a peer-reflexive candidate over the relay, which LiveKit disables for Firefox
  (`SupportsPrflxOverRelay`). Too fragile to promise; we publish the port instead.

Cloudflare's credentials are not the TURN REST API's: its API generates each client's username
and password (`POST /v1/turn/keys/<key id>/credentials/generate-ice-servers` with the key's
API token), and LiveKit's `turn_servers` takes only a shared secret or a fixed pair, so LiveKit
cannot hand them out.

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| Point LiveKit's `turn_servers` at the provider (no code) | Clients unchanged | Works only for shared-secret providers; Cloudflare's per-client credentials cannot be configured; a fixed pair would be shared by every client | Rejected |
| Chat and the gateway ask the provider for each participant's credentials and put the ICE servers in the ticket; LiveKit names no relay | One path for every provider; credentials exist only for a participant the room admitted; the provider's API token stays in our pods | Clients pass the ICE servers to their peer connection; a long call outlives its credential and needs a new ticket | Accepted |
| Keep STUNner and restrict its port to known clients | No new moving part | Clients come from anywhere; no list to restrict to | Rejected |
| Make LiveKit reach the relay outbound only (no published port) | Truly no inbound port | Depends on the node's NAT keeping ports, and fails in Firefox when it does not (above) | Rejected; documented as unsupported |

## Decision

- `ULW_TURN_MODE` is `self-hosted` (the default: STUNner, unchanged) or `external`, read by chat
  for call tickets and by the gateway for a live stream's publisher tickets
  (`infra/turn`). `ULW_TURN_PROVIDER` is `hmac` (the TURN REST API's credentials: username
  `<expiry>:<device id>`, password the base64 HMAC-SHA1 of it under `ULW_TURN_SHARED_SECRET`,
  for `ULW_TURN_URLS`; coturn, eturnal and STUNner check these) or `cloudflare` (its
  generate-ice-servers API, `ULW_TURN_KEY_ID` and `ULW_TURN_KEY_API_TOKEN`). URIs on port 53 are
  dropped from Cloudflare's answer: browsers may block that port and then wait on it. The
  credential lives `ULW_TURN_TTL_SECONDS` (4 h by default, as ADR-0037's). The device id, not
  the user, names an hmac credential, since the provider logs usernames.
- The media port carries it: a `MediaTicket` has `ice_servers`, which a wrapper around the SFU
  (`make_relayed_sfu`) fills after the room admits the participant, so a refused join costs the
  provider nothing; a ticket whose credentials could not be issued is not issued (`unavailable`),
  since its client could not reach the media. Tickets carry `ice_servers` as RTCIceServer
  objects; clients pass them as `rtcConfig.iceServers` with `iceTransportPolicy: "relay"`
  (`livekit-client` uses its own `rtcConfig.iceServers` over the join response's).
- A call ticket reaches the asking node as the room's owner's answer (ADR-0087), and that answer
  carries `ice_servers`: every server's URIs, username and credential, in call layout 3. Layout 2,
  v1.1.0's, had no place for them, so the owner's relays were dropped on the way and no call
  connected in external mode; v1.1.1 fixed it. A node of layout 2 refuses layout 3's answers and
  the other way round, rather than hand a client a ticket without its relays.
- `components/external-turn` turns it on in an overlay: it deletes STUNner's Gateway and UDPRoute
  (so neither STUNner's operator nor `stunner-secrets` is needed), runs LiveKit with
  `--node-ip=<MEDIA_PUBLIC_IP>`, `udp_port: <MEDIA_UDP_PORT>`, `advertise_internal_ip: true` (egress
  still reaches the pod from inside the cluster) and no `turn_servers`, and publishes
  `MEDIA_UDP_PORT` as a LoadBalancer with `externalTrafficPolicy: Local`, admitting the provider's
  relay ranges only, in its `loadBalancerSourceRanges` and in a NetworkPolicy alike. The ranges
  shipped are Cloudflare's published IPv4 ranges; Cloudflare does not publish its relays' ranges
  apart, so the runbook has the operator confirm a relay address with `turn_probe.py`.
- `deploy/local/validate-manifests.sh` renders both example overlays with the component and checks
  them (`check-external-turn.py`): STUNner gone, both services in external mode, LiveKit's
  configuration base's except `rtc`, every `$(NAME)` defined before use, the two source lists
  equal, and one media port throughout.

## Consequences

- With `external`, no port is open to the whole internet: LiveKit's media port admits the
  provider's ranges only, so a flood from anywhere else is dropped at the node's firewall or the
  pod's policy. The node's public address is still disclosed to every participant (above), and a
  flood large enough to fill the link needs no open port; only an address that is not the
  website's would take the site out of that path (a second public address, or LiveKit on another
  host). For Cloudflare, the allowed sources are all of Cloudflare's ranges, not only its relays.
- Clients must pass the ticket's `ice_servers`. A client that ignores them gets LiveKit's default
  STUN server, cannot reach the published port, and fails ICE. WHIP encoders that read ICE servers
  only from the WHIP answer's `Link` headers (OBS) find none there; browser publishers pass the
  ticket's.
- A credential cannot be renewed mid-call: a call longer than `ULW_TURN_TTL_SECONDS` loses its
  relay at the next allocation refresh, and the client asks chat for a new ticket (the documented
  rejoin). Cloudflare can revoke a credential by username; nothing here does yet.
- Each ticket costs one provider API call with `cloudflare` (none with `hmac`); its outage makes
  tickets `unavailable`, as LiveKit's does.
- Two environments on one node need a `MEDIA_UDP_PORT` each, as they needed a `TURN_PORT` each.
- Moving to layout 3 is a rolling update like layout 2's (ADR-0091): while nodes of both run, a
  ticket or call move asked of an owner on the other layout is answered `unavailable` with
  `retry_after_ms`, and the client asks again; once every node runs v1.1.1 or later, asks pass
  as before, in either TURN mode. Turn external mode on only after that.
- Reopen if LiveKit gains a TURN client for its own candidates (then no port is published at
  all), if a provider publishes its relay ranges (narrow the lists), or if the media plane moves
  to an address of its own.
