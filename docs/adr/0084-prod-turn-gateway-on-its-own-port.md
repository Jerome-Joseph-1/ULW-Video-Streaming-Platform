# 0084. Prod's TURN Gateway on its own port, sharing the cluster's STUNner configuration

Status: Accepted, amended by 0088 (the TURN port is `TURN_PORT`, an operator's choice)
Date: 2026-10-03
Amends: ADR-0037 ("One Gateway, one listener, `TURN-UDP` on 3478": that is now stage's Gateway;
prod has one of its own on 3479)

## Context

ADR-0037 gave the cluster one STUNner Gateway, with one `TURN-UDP` listener on 3478, in
`apps-stage`, and noted that a stage credential would pass authentication "on the prod listener
once prod has one". Prod now needs the realtime plane (one-to-one calls, and live ingest over
WHIP, ADR-0053). Stage and prod run on k8s-prod's one node. STUNner publishes each Gateway as a
LoadBalancer Service, which K3s's ServiceLB binds on the node's address, so a second Gateway on
UDP 3478 would stay pending: the port is stage's. Prod's relay must also reach prod's LiveKit
only, never stage's.

## Options

| Option | For | Against / verdict |
|---|---|---|
| One Gateway for both: a prod UDPRoute on stage's Gateway, to prod's LiveKit | One public port; nothing new to open | Rejected: the Gateway lives in `apps-stage` and admits routes from its own namespace only; opening it to `apps` makes one relay serve both environments, so a stage credential reaches prod's LiveKit through stage's relay and a stage rollout of STUNner interrupts prod's calls |
| A prod Gateway on 3478 at a second public address (`spec.addresses`) | The well-known TURN port for both | Rejected for now: needs a second public address on the node, which Askedin has not got; kept as the alternative RUNBOOK step 7 describes |
| A prod Gateway on UDP 3479, same address, same GatewayClass and GatewayConfig | Nothing Askedin lacks: one more firewall port; prod's relay reaches prod's LiveKit only; stunnerd per environment | Accepted |
| A prod GatewayConfig and secret of its own | A stage credential would not authenticate on prod's listener | Rejected for now: STUNner's GatewayClass names one GatewayConfig, so this needs a second class as well; ADR-0037's consequence already accepts that a stage credential reaches only prod LiveKit's UDP port, which still needs a ticket to do anything |

## Decision

- Prod has its own Gateway, `stunner` in `apps` (`deploy/askedin/overlays/prod/stunner/`), one
  `TURN-UDP` listener on **3479**, `externalTrafficPolicy: Local`, and a UDPRoute to prod's
  LiveKit on 7882. Stage's stays on 3478, unchanged.
- Both Gateways name `stunner-gatewayclass`, so they share the cluster's GatewayConfig, its
  `ephemeral` auth and the one `stunner-secrets` (ADR-0037), and the one `Dataplane`, so prod's
  stunnerd is sized as stage's.
- Prod's LiveKit hands clients `$(TURN_HOST):3479` in `rtc.turn_servers`; `TURN_HOST` is prod's
  own value in `sfu-secrets` in `apps`, and may be the same address as stage's.
- Askedin opens UDP 3479 on the node's firewall for prod, as 3478 for stage (RUNBOOK step 7,
  "Prod").

## Consequences

- Two public UDP ports on the node, one per environment; clients behind firewalls that admit
  only 3478 reach stage's relay and not prod's. Monitor ICE failure rate by environment.
- A stage credential still authenticates on prod's listener, and the reverse, since the secret
  is shared; each relays only to its own environment's LiveKit.
- Rotating `TURN_SECRET` rotates it for both environments at once, as before.
- A rollout or deletion of either Gateway touches only its own environment's media.
- Reopen when the node gets a second public address (prod back to 3478 on it), or when the
  environments move to separate clusters or need separate TURN secrets.
