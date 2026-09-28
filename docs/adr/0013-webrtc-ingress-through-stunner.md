# 0013. WebRTC media ingresses through STUNner

Status: Accepted
Date: 2026-09-28

## Context

The cluster's front door is Envoy Gateway on K3s, which terminates TLS for HTTP and WebSocket.
WebRTC media is UDP, and ICE only works if the SFU sees, and answers, the addresses the browser
really uses. Envoy Gateway supports `UDPRoute`, but proxies it non-transparently: the backend
sees the Gateway's IP as the source, the SFU's connectivity checks go to the wrong address, and
ICE fails. Pods may not use `hostNetwork` or run privileged.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Envoy Gateway `UDPRoute` | Same Gateway and CRDs as the HTTP routes; nothing new to run | Rejected: non-transparent; the SFU sees the Gateway's IP and ICE breaks |
| SFU with `hostNetwork: true` and a public UDP port range | Direct media path; the SFU sees real client addresses; the usual way to run an SFU on Kubernetes | Rejected: gives a pod the host's network namespace, which the cluster does not allow |
| STUNner as a TURN gateway in front of the SFU | Gateway API native; ICE succeeds through TURN; a documented LiveKit integration; no host networking | Accepted |

## Decision

- WebRTC media enters through STUNner, under its own GatewayClass `stunner-gatewayclass` and its
  own `UDPRoute` CRD in `stunner.l7mp.io/v1`, routed to the SFU's service. Envoy Gateway keeps
  HTTP and WebSocket traffic.
- Clients authenticate to STUNner with TURN long-term credentials, configured through the
  GatewayConfig `authRef`. Clients receive them only after authenticating (ADR-0018).
- No `hostNetwork`, no privileged pods.

## Consequences

- Every media packet takes one extra in-cluster hop (browser to STUNner to SFU) and carries TURN
  framing (a 4-byte ChannelData header). Both fit inside the 10% headroom of ADR-0012.
- STUNner relays only to the backends listed in its `UDPRoute`, so a leaked credential reaches
  the SFU's port and nothing else; it is not an open relay.
- Long-term credentials are static. Rotating them means updating the referenced Secret, and
  clients holding the old ones fail to allocate until they fetch new ones.
- Two Gateway API implementations share the cluster; each route must name the right class.
- Monitor ICE failure rate by client network and TURN allocation counts.
- Reopen if Envoy Gateway gains a transparent UDP mode, or if the no-`hostNetwork` rule is lifted
  and the extra hop shows up in call latency.
