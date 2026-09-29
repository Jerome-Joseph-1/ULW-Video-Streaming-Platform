#!/usr/bin/env python3
"""A STUN and TURN client for one server, standard library only, printing what it saw as JSON.
tests/cluster/stunner_check.py runs it in a container on the sandbox's outside network; it
can equally be run from any machine against a real TURN server.

    turn_probe.py HOST PORT                  a Binding request only
    TURN_SECRET=... turn_probe.py HOST PORT [--user U] [--permit IP] [--forbid IP]

With TURN_SECRET in its environment (STUNner's ephemeral auth shared secret, ADR-0033; never
on the command line, where shell history and the process list keep it) it also allocates a relay
with a credential minted from it, asks for a permission to each --permit and --forbid peer,
releases the allocation, and tries two allocations that must fail: a wrong password and a
correctly signed credential that has expired.

Each exchange uses a fresh socket, so each allocation has its own 5-tuple.
"""
import argparse
import ipaddress
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stun  # noqa: E402

# RFC 5389 section 7.2.1: a request over UDP is sent at 0, RTO, 3 RTO, 7 RTO... with RTO
# 500 ms, Rc = 7 sends, and a last wait of Rm = 16 RTO, 39.5 s in all.
RTO_S = 0.5
SENDS = 7
LAST_WAIT_RTO = 16
# A credential whose expiry passed this long ago: past any clock skew between here and the
# server, so only a server that ignores the expiry accepts it.
EXPIRED_BY_S = 3600
# How long the credentials minted here stay valid: long enough for one run.
WINDOW_S = 600


class NoAnswer(Exception):
    pass


def transact(sock, method, attributes, key=None):
    """Sends a request and returns the response with its transaction id, retransmitting on the
    RFC's schedule; datagrams for other transactions are dropped."""
    transaction_id = os.urandom(12)
    request = stun.encode(method, stun.REQUEST, transaction_id, attributes, key=key)
    waits = [RTO_S * 2 ** i for i in range(SENDS - 1)] + [RTO_S * LAST_WAIT_RTO]
    for wait in waits:
        sock.send(request)
        deadline = time.monotonic() + wait
        while (left := deadline - time.monotonic()) > 0:
            sock.settimeout(left)
            try:
                data = sock.recv(2048)
            except socket.timeout:
                break
            try:
                response = stun.decode(data)
            except stun.Malformed:
                continue
            if response.transaction_id == transaction_id and response.method == method:
                return response
    raise NoAnswer(f"no answer to method {method:#x} after {sum(waits):.1f} s")


def fresh(host, port):
    sock = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET, socket.SOCK_DGRAM)
    sock.connect((host, port))
    return sock


def outcome(response, key=None):
    result = {"class": {stun.SUCCESS: "success", stun.ERROR: "error"}.get(response.cls, "other")}
    if response.cls == stun.ERROR:
        result["error"] = list(response.error() or (0, ""))
    if key is not None and response.cls == stun.SUCCESS:
        # A success signed with our key is proof the server derived the same key: it holds
        # the shared secret, not merely something that answers 200 to anyone.
        result["integrity"] = stun.integrity_ok(response, key)
    return result


def binding(host, port):
    with fresh(host, port) as sock:
        local = sock.getsockname()[:2]
        response = transact(sock, stun.BINDING, [])
        mapped = response.xor_address(stun.XOR_MAPPED_ADDRESS)
        return {"local": list(local), "mapped": list(mapped) if mapped else None,
                **outcome(response)}


def allocate(sock, username, password):
    """Allocate with long-term credentials (RFC 8656 section 7.1): the first request carries
    none and draws a 401 with the realm and nonce to sign the second with. Returns the
    response and the key."""
    transport = [(stun.REQUESTED_TRANSPORT, bytes([stun.UDP_TRANSPORT, 0, 0, 0]))]
    challenge = transact(sock, stun.ALLOCATE, transport)
    if challenge.cls != stun.ERROR or (challenge.error() or (0,))[0] != 401:
        raise NoAnswer(f"unauthenticated Allocate got {outcome(challenge)}, not a 401")
    realm, nonce = challenge.get(stun.REALM), challenge.get(stun.NONCE)
    key = stun.long_term_key(username, realm.decode(), password)
    signed = [(stun.USERNAME, username.encode()), (stun.REALM, realm), (stun.NONCE, nonce)]
    response = transact(sock, stun.ALLOCATE, transport + signed, key=key)
    return response, key, signed


def turn(host, port, secret, user, permit, forbid):
    report = {}
    username, password = stun.ephemeral_credentials(secret, user, time.time() + WINDOW_S)
    with fresh(host, port) as sock:
        local = sock.getsockname()[:2]
        response, key, signed = allocate(sock, username, password)
        report["allocate"] = {"local": list(local), **outcome(response, key)}
        if response.cls == stun.SUCCESS:
            for name, kind in (("mapped", stun.XOR_MAPPED_ADDRESS),
                               ("relayed", stun.XOR_RELAYED_ADDRESS)):
                address = response.xor_address(kind)
                report["allocate"][name] = list(address) if address else None
            for label, peers in (("permit", permit), ("forbid", forbid)):
                for peer in peers:
                    # A permission is per IP address, the port ignored (RFC 8656 section 9).
                    # The transaction id enters an XORed address only for IPv6, and peers
                    # are IPv4 (main).
                    peer_attr = stun.encode_xor_address(peer, 0, bytes(12))
                    answer = transact(sock, stun.CREATE_PERMISSION,
                                      [(stun.XOR_PEER_ADDRESS, peer_attr)] + signed, key=key)
                    report.setdefault(label, {})[peer] = outcome(answer, key)
            released = transact(sock, stun.REFRESH,
                                [(stun.LIFETIME, (0).to_bytes(4, "big"))] + signed, key=key)
            report["release"] = outcome(released, key)
    with fresh(host, port) as sock:
        response, _, _ = allocate(sock, username, password + "x")
        report["wrong_password"] = outcome(response)
    expired_name, expired_password = stun.ephemeral_credentials(
        secret, user, time.time() - EXPIRED_BY_S)
    with fresh(host, port) as sock:
        response, _, _ = allocate(sock, expired_name, expired_password)
        report["expired"] = outcome(response)
    return report


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--user", default="probe")
    ipv4 = lambda v: str(ipaddress.IPv4Address(v))  # noqa: E731
    parser.add_argument("--permit", action="append", default=[], type=ipv4)
    parser.add_argument("--forbid", action="append", default=[], type=ipv4)
    args = parser.parse_args(argv)
    try:
        report = {"binding": binding(args.host, args.port)}
        secret = os.environ.get("TURN_SECRET")
        if secret:
            report.update(turn(args.host, args.port, secret, args.user, args.permit,
                               args.forbid))
    except NoAnswer as e:
        sys.exit(f"turn_probe: {e}")
    json.dump(report, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main(sys.argv[1:])
