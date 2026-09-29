"""The STUN (RFC 5389) and TURN (RFC 8656) messages the STUNner checks send and read, built and
parsed by hand, standard library only: turn_probe.py runs it in a bare Python container.

Only what the checks use: Binding, Allocate, Refresh and CreatePermission; IPv4 and IPv6
addresses; long-term credentials (RFC 5389 section 10.2) and FINGERPRINT.
"""
import base64
import binascii
import hashlib
import hmac
import ipaddress
import struct

MAGIC_COOKIE = 0x2112A442
HEADER = struct.Struct("!HHI12s")

BINDING = 0x001
ALLOCATE = 0x003
REFRESH = 0x004
CREATE_PERMISSION = 0x008

REQUEST = 0b00
INDICATION = 0b01
SUCCESS = 0b10
ERROR = 0b11

MAPPED_ADDRESS = 0x0001
USERNAME = 0x0006
MESSAGE_INTEGRITY = 0x0008
ERROR_CODE = 0x0009
LIFETIME = 0x000D
XOR_PEER_ADDRESS = 0x0012
REALM = 0x0014
NONCE = 0x0015
XOR_RELAYED_ADDRESS = 0x0016
REQUESTED_TRANSPORT = 0x0019
XOR_MAPPED_ADDRESS = 0x0020
SOFTWARE = 0x8022
FINGERPRINT = 0x8028

# RFC 5389 section 15.5: the CRC-32 is XORed with this ("STUN" in ASCII) so a STUN FINGERPRINT
# never matches the CRC another protocol sharing the port might carry.
FINGERPRINT_XOR = 0x5354554E
# REQUESTED-TRANSPORT's protocol number for UDP (IANA), in the value's first byte.
UDP_TRANSPORT = 17


class Malformed(ValueError):
    pass


def message_type(method, cls):
    # The class's two bits sit at positions 4 and 8, between the method's bits (section 6).
    return ((method & 0xF80) << 2) | ((method & 0x070) << 1) | (method & 0x00F) \
        | ((cls & 0b10) << 7) | ((cls & 0b01) << 4)


def split_type(value):
    method = ((value >> 2) & 0xF80) | ((value >> 1) & 0x070) | (value & 0x00F)
    cls = ((value >> 7) & 0b10) | ((value >> 4) & 0b01)
    return method, cls


def _pad(length):
    return -length % 4


def _attribute(kind, value):
    return struct.pack("!HH", kind, len(value)) + value + b"\0" * _pad(len(value))


class Message:
    def __init__(self, method, cls, transaction_id, attributes, raw=b""):
        self.method = method
        self.cls = cls
        self.transaction_id = transaction_id
        self.attributes = attributes
        self.raw = raw

    def get(self, kind):
        return next((v for k, v in self.attributes if k == kind), None)

    def xor_address(self, kind):
        value = self.get(kind)
        return None if value is None else decode_xor_address(value, self.transaction_id)

    def error(self):
        """(code, reason) of an ERROR-CODE attribute (section 15.6), or None."""
        value = self.get(ERROR_CODE)
        if value is None:
            return None
        if len(value) < 4:
            raise Malformed("short ERROR-CODE")
        return (value[2] & 0x7) * 100 + value[3], value[4:].decode("utf-8", "replace")


def long_term_key(username, realm, password):
    # Section 15.4: MD5 over "username:realm:password". SASLprep is the identity on the ASCII
    # names these checks use.
    return hashlib.md5(f"{username}:{realm}:{password}".encode()).digest()


def ephemeral_credentials(secret, user, expires_at):
    """A time-windowed TURN credential ("REST API for access to TURN services",
    draft-uberti-behave-turn-rest-00), the form STUNner's ephemeral auth checks: the username is
    the expiry in Unix seconds and the user, the password the base64 HMAC-SHA1 of the username
    under the shared secret."""
    username = f"{int(expires_at)}:{user}"
    digest = hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()
    return username, base64.b64encode(digest).decode()


def encode_xor_address(host, port, transaction_id):
    ip = ipaddress.ip_address(host)
    family = 0x01 if ip.version == 4 else 0x02
    key = struct.pack("!I", MAGIC_COOKIE) + (transaction_id if ip.version == 6 else b"")
    packed = bytes(a ^ b for a, b in zip(ip.packed, key))
    return struct.pack("!BBH", 0, family, port ^ (MAGIC_COOKIE >> 16)) + packed


def decode_xor_address(value, transaction_id):
    if len(value) < 4:
        raise Malformed("short address")
    family, xport = value[1], struct.unpack_from("!H", value, 2)[0]
    size = {0x01: 4, 0x02: 16}.get(family)
    if size is None or len(value) != 4 + size:
        raise Malformed(f"address family {family:#x} with {len(value) - 4} bytes")
    key = struct.pack("!I", MAGIC_COOKIE) + transaction_id
    packed = bytes(a ^ b for a, b in zip(value[4:], key))
    return str(ipaddress.ip_address(packed)), xport ^ (MAGIC_COOKIE >> 16)


def encode(method, cls, transaction_id, attributes, key=None, fingerprint=True):
    """The message's bytes. With `key`, MESSAGE-INTEGRITY follows the attributes, and
    FINGERPRINT comes last either way when asked for (section 15)."""
    if len(transaction_id) != 12:
        raise ValueError("a transaction id is 96 bits")
    body = b"".join(_attribute(k, v) for k, v in attributes)
    kind = message_type(method, cls)

    def header(length):
        return HEADER.pack(kind, length, MAGIC_COOKIE, transaction_id)

    if key is not None:
        # The HMAC covers the header with its length already counting MESSAGE-INTEGRITY
        # itself (4 + 20 bytes), but not anything after it (section 15.4).
        mac = hmac.new(key, header(len(body) + 24) + body, hashlib.sha1).digest()
        body += _attribute(MESSAGE_INTEGRITY, mac)
    if fingerprint:
        # Likewise the CRC counts FINGERPRINT's own 8 bytes in the length (section 15.5).
        crc = binascii.crc32(header(len(body) + 8) + body) ^ FINGERPRINT_XOR
        body += _attribute(FINGERPRINT, struct.pack("!I", crc))
    return header(len(body)) + body


def decode(data):
    if len(data) < HEADER.size:
        raise Malformed(f"{len(data)} bytes is shorter than a header")
    kind, length, cookie, transaction_id = HEADER.unpack_from(data)
    if kind & 0xC000:
        raise Malformed("top bits set: not STUN")
    if cookie != MAGIC_COOKIE:
        raise Malformed(f"magic cookie {cookie:#x}")
    if length % 4 or HEADER.size + length != len(data):
        raise Malformed(f"length {length} for a {len(data)}-byte datagram")
    attributes = []
    offset = HEADER.size
    while offset < len(data):
        if offset + 4 > len(data):
            raise Malformed("truncated attribute header")
        attr, size = struct.unpack_from("!HH", data, offset)
        end = offset + 4 + size
        if end > len(data):
            raise Malformed(f"attribute {attr:#06x} runs past the message")
        attributes.append((attr, data[offset + 4:end]))
        offset = end + _pad(size)
    method, cls = split_type(kind)
    return Message(method, cls, transaction_id, attributes, bytes(data))


def _offset_of(message, kind):
    offset = HEADER.size
    for attr, value in message.attributes:
        if attr == kind:
            return offset
        offset += 4 + len(value) + _pad(len(value))
    return None


def integrity_ok(message, key):
    """Whether MESSAGE-INTEGRITY is present and matches `key`."""
    offset = _offset_of(message, MESSAGE_INTEGRITY)
    if offset is None:
        return False
    covered = bytearray(message.raw[:offset])
    struct.pack_into("!H", covered, 2, offset - HEADER.size + 24)
    mac = hmac.new(key, bytes(covered), hashlib.sha1).digest()
    return hmac.compare_digest(mac, message.get(MESSAGE_INTEGRITY))


def fingerprint_ok(message):
    offset = _offset_of(message, FINGERPRINT)
    if offset is None or offset + 8 != len(message.raw):
        return False
    crc = binascii.crc32(message.raw[:offset]) ^ FINGERPRINT_XOR
    return struct.pack("!I", crc) == message.get(FINGERPRINT)
